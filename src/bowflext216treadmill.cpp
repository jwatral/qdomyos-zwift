#include "bowflext216treadmill.h"
#include "keepawakehelper.h"
#include "virtualtreadmill.h"
#include <QBluetoothLocalDevice>
#include <QDateTime>
#include <QFile>
#include <QMetaEnum>
#include <QSettings>
#include <chrono>

using namespace std::chrono_literals;

#ifdef Q_OS_IOS
extern quint8 QZ_EnableDiscoveryCharsAndDescripttors;
#endif

bowflext216treadmill::bowflext216treadmill(uint32_t pollDeviceTime, bool noConsole, bool noHeartService,
                                           double forceInitSpeed, double forceInitInclination) {

#ifdef Q_OS_IOS
    QZ_EnableDiscoveryCharsAndDescripttors = true;
#endif

    m_watt.setType(metric::METRIC_WATT);
    Speed.setType(metric::METRIC_SPEED);
    this->noConsole = noConsole;
    this->noHeartService = noHeartService;

    if (forceInitSpeed > 0)
        lastSpeed = forceInitSpeed;

    if (forceInitInclination > 0)
        lastInclination = forceInitInclination;

    refresh = new QTimer(this);
    initDone = false;
    connect(refresh, &QTimer::timeout, this, &bowflext216treadmill::update);
    refresh->start(500ms);
}

void bowflext216treadmill::writeCharacteristic(uint8_t *data, uint8_t data_len, const QString &info, bool disable_log,
                                               bool wait_for_response) {
    QEventLoop loop;
    QTimer timeout;

    if (wait_for_response) {
        connect(this, &bowflext216treadmill::packetReceived, &loop, &QEventLoop::quit);
        timeout.singleShot(300ms, &loop, &QEventLoop::quit);
    } else {
        // connect(gattCommunicationChannelService, &QLowEnergyService::characteristicWritten, &loop,
        // &QEventLoop::quit); timeout.singleShot(300ms, &loop, &QEventLoop::quit);
    }

    gattCommunicationChannelService->writeCharacteristic(gattWriteCharacteristic,
                                                         QByteArray((const char *)data, data_len));

    if (!disable_log) {
        emit debug(QStringLiteral(" >> ") + QByteArray((const char *)data, data_len).toHex(' ') +
                   QStringLiteral(" // ") + info);
    }

    // packets sent from the characChanged event, i don't want to block everything
    if (wait_for_response) {
        loop.exec();

        if (timeout.isActive() == false)
            emit debug(QStringLiteral(" exit for timeout"));
    }
}

void bowflext216treadmill::updateDisplay(uint16_t elapsed) {}

void bowflext216treadmill::forceIncline(double requestIncline) {}

double bowflext216treadmill::minStepInclination() { return 1.0; }

void bowflext216treadmill::forceSpeed(double requestSpeed) {}

void bowflext216treadmill::update() {
    if (m_control->state() == QLowEnergyController::UnconnectedState) {
        emit disconnected();
        return;
    }

    qDebug() << m_control->state() << bluetoothDevice.isValid() << gattCommunicationChannelService
             << gattWriteCharacteristic.isValid() << initDone << requestSpeed << requestInclination;

    if (initRequest) {
        initRequest = false;
        btinit((lastSpeed > 0 ? true : false));
    } else if (bluetoothDevice.isValid() && m_control->state() == QLowEnergyController::DiscoveredState &&
               gattCommunicationChannelService && gattWriteCharacteristic.isValid() && initDone) {
        QSettings settings;
        // ******************************************* virtual treadmill init *************************************
        if (!firstInit && !virtualTreadMill) {
            bool virtual_device_enabled = settings.value(QStringLiteral("virtual_device_enabled"), true).toBool();
            if (virtual_device_enabled) {
                emit debug(QStringLiteral("creating virtual treadmill interface..."));
                virtualTreadMill = new virtualtreadmill(this, noHeartService);
                connect(virtualTreadMill, &virtualtreadmill::debug, this, &bowflext216treadmill::debug);
                firstInit = 1;
            }
        }
        // ********************************************************************************************************

        update_metrics(true, watts(settings.value(QStringLiteral("weight"), 75.0).toFloat()));

        // updating the treadmill console every second
        // it seems that stops the communication
        if (sec1Update++ >= (1000 / refresh->interval())) {
            updateDisplay(elapsed.value());
        }

        if (requestSpeed != -1) {
            if (requestSpeed != currentSpeed().value() && requestSpeed >= 0 && requestSpeed <= 22) {
                emit debug(QStringLiteral("writing speed ") + QString::number(requestSpeed));
                // double inc = Inclination.value(); // NOTE: clang-analyzer-deadcode.DeadStores
                if (requestInclination != -1) {
                    //                        inc = requestInclination;
                    requestInclination = -1;
                }
                forceSpeed(requestSpeed);
            }
            requestSpeed = -1;
        }
        if (requestInclination != -1) {
            if (requestInclination != currentInclination().value() && requestInclination >= 0 &&
                requestInclination <= 15) {
                emit debug(QStringLiteral("writing incline ") + QString::number(requestInclination));
                // double speed = currentSpeed().value(); // NOTE: clang-analyzer-deadcode.DeadStores
                if (requestSpeed != -1) {
                    // speed = requestSpeed;
                    requestSpeed = -1;
                }
                forceIncline(requestInclination);
            }
            requestInclination = -1;
        }

        if (requestStart != -1) {
            uint8_t start[] = {0x07, 0x06, 0xcf, 0x00, 0x1f, 0x05, 0x00};
            emit debug(QStringLiteral("starting..."));
            if (lastSpeed == 0.0) {
                lastSpeed = 0.5;
            }
            writeCharacteristic(start, sizeof(start), QStringLiteral("start"), false, true);
            requestStart = -1;
            emit tapeStarted();
        }
        if (requestStop != -1) {
            uint8_t stop[] = {0x0a, 0x08, 0x7a, 0x00, 0x19, 0x28, 0x01, 0x32, 0x00, 0x00};
            emit debug(QStringLiteral("stopping..."));
            writeCharacteristic(stop, sizeof(stop), QStringLiteral("stop"), false, true);
            requestStop = -1;
        }
    }
}

void bowflext216treadmill::serviceDiscovered(const QBluetoothUuid &gatt) {
    emit debug(QStringLiteral("serviceDiscovered ") + gatt.toString());
}

void bowflext216treadmill::characteristicChanged(const QLowEnergyCharacteristic &characteristic,
                                                 const QByteArray &newValue) {
    // qDebug() << "characteristicChanged" << characteristic.uuid() << newValue << newValue.length();
    QSettings settings;
    QString heartRateBeltName =
        settings.value(QStringLiteral("heart_rate_belt_name"), QStringLiteral("Disabled")).toString();
    Q_UNUSED(characteristic);
    QByteArray value = newValue;

    emit debug(QStringLiteral(" << ") + QString::number(value.length()) + QStringLiteral(" ") + value.toHex(' '));

    emit packetReceived();

    if (characteristic.uuid() != gattNotify3Characteristic.uuid())
        return;

    if ((newValue.length() != 20))
        return;

    double speed = GetSpeedFromPacket(value);
    double incline = GetInclinationFromPacket(value);
    // double kcal = GetKcalFromPacket(value);
    // double distance = GetDistanceFromPacket(value);

#ifdef Q_OS_ANDROID
    if (settings.value("ant_heart", false).toBool())
        Heart = (uint8_t)KeepAwakeHelper::heart();
    else
#endif
    {
        /*if(heartRateBeltName.startsWith("Disabled"))
        Heart = value.at(18);*/
    }
    emit debug(QStringLiteral("Current speed: ") + QString::number(speed));
    // emit debug(QStringLiteral("Current incline: ") + QString::number(incline));
    // emit debug(QStringLiteral("Current KCal: ") + QString::number(kcal));
    // debug("Current Distance: " + QString::number(distance));

    if (Speed.value() != speed) {
        emit speedChanged(speed);
    }
    Speed = speed;
    if (Inclination.value() != incline) {
        emit inclinationChanged(0.0, incline);
    }
    Inclination = incline;

    // KCal = kcal;
    // Distance = distance;

    if (speed > 0) {
        lastSpeed = speed;
        lastInclination = incline;
    }

    if (!firstCharacteristicChanged) {
        if (watts(settings.value(QStringLiteral("weight"), 75.0).toFloat()))
            KCal +=
                ((((0.048 * ((double)watts(settings.value(QStringLiteral("weight"), 75.0).toFloat())) + 1.19) *
                   settings.value(QStringLiteral("weight"), 75.0).toFloat() * 3.5) /
                  200.0) /
                 (60000.0 / ((double)lastTimeCharacteristicChanged.msecsTo(
                                QDateTime::currentDateTime())))); //(( (0.048* Output in watts +1.19) * body weight in
                                                                  // kg * 3.5) / 200 ) / 60

        Distance += ((Speed.value() / 3600.0) /
                     (1000.0 / (lastTimeCharacteristicChanged.msecsTo(QDateTime::currentDateTime()))));
    }

    emit debug(QStringLiteral("Current Distance Calculated: ") + QString::number(Distance.value()));

    if (m_control->error() != QLowEnergyController::NoError) {
        qDebug() << QStringLiteral("QLowEnergyController ERROR!!") << m_control->errorString();
    }

    lastTimeCharacteristicChanged = QDateTime::currentDateTime();
    firstCharacteristicChanged = false;
}

double bowflext216treadmill::GetSpeedFromPacket(const QByteArray &packet) {
    uint16_t convertedData = (packet.at(7) << 10) | packet.at(9);
    double data = (double)convertedData / 100.0f;
    return data;
}

double bowflext216treadmill::GetKcalFromPacket(const QByteArray &packet) {
    uint16_t convertedData = (packet.at(7) << 8) | packet.at(8);
    return (double)convertedData;
}

double bowflext216treadmill::GetDistanceFromPacket(const QByteArray &packet) {
    uint16_t convertedData = (packet.at(12) << 8) | packet.at(13);
    double data = ((double)convertedData) / 10.0f;
    return data;
}

double bowflext216treadmill::GetInclinationFromPacket(const QByteArray &packet) {
    uint16_t convertedData = packet.at(4);
    double data = convertedData;

    return data;
}

void bowflext216treadmill::btinit(bool startTape) {
    Q_UNUSED(startTape)
    uint8_t initData1[] = {0x05, 0x01, 0xe3, 0x00, 0x17};
    uint8_t initData2[] = {0x05, 0x02, 0xe4, 0x00, 0x15};
    uint8_t initData3[] = {0x05, 0x03, 0xe1, 0x00, 0x17};
    uint8_t initData4[] = {0x07, 0x04, 0xd1, 0x00, 0x1f, 0x05, 0x00};
    uint8_t initData5[] = {0x05, 0x05, 0xdf, 0x00, 0x17};

    writeCharacteristic(initData1, sizeof(initData1), QStringLiteral("init"), false, true);
    writeCharacteristic(initData2, sizeof(initData2), QStringLiteral("init"), false, true);
    writeCharacteristic(initData3, sizeof(initData3), QStringLiteral("init"), false, true);
    writeCharacteristic(initData4, sizeof(initData4), QStringLiteral("init"), false, true);
    writeCharacteristic(initData5, sizeof(initData5), QStringLiteral("init"), false, true);

    initDone = true;
}

void bowflext216treadmill::stateChanged(QLowEnergyService::ServiceState state) {
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceState>();
    emit debug(QStringLiteral("BTLE stateChanged ") + QString::fromLocal8Bit(metaEnum.valueToKey(state)));
    if (state == QLowEnergyService::ServiceDiscovered) {
        QBluetoothUuid _gattWriteCharacteristicId(QStringLiteral("1717b3c0-9803-11e3-90e1-0002a5d5c51b"));
        QBluetoothUuid _gattNotify1CharacteristicId(QStringLiteral("35ddd0a09-8031-1e39-a8b0-002a5d5c51b"));
        QBluetoothUuid _gattNotify2CharacteristicId(QStringLiteral("6be8f5809-8031-1e3a-b030-002a5d5c51b"));
        QBluetoothUuid _gattNotify3CharacteristicId(QStringLiteral("a46a4a809-8031-1e38-f3c0-002a5d5c51b"));
        QBluetoothUuid _gattNotify4CharacteristicId(QStringLiteral("b8066ec09-8031-1e38-3460-002a5d5c51b"));
        QBluetoothUuid _gattNotify5CharacteristicId(QStringLiteral("d57cda209-8031-1e38-4260-002a5d5c51b"));

        // qDebug() << gattCommunicationChannelService->characteristics();

        gattWriteCharacteristic = gattCommunicationChannelService->characteristic(_gattWriteCharacteristicId);
        gattNotify1Characteristic = gattCommunicationChannelService->characteristic(_gattNotify1CharacteristicId);
        gattNotify2Characteristic = gattCommunicationChannelService->characteristic(_gattNotify2CharacteristicId);
        gattNotify3Characteristic = gattCommunicationChannelService->characteristic(_gattNotify3CharacteristicId);
        gattNotify4Characteristic = gattCommunicationChannelService->characteristic(_gattNotify4CharacteristicId);
        gattNotify5Characteristic = gattCommunicationChannelService->characteristic(_gattNotify5CharacteristicId);
        Q_ASSERT(gattWriteCharacteristic.isValid());
        Q_ASSERT(gattNotify1Characteristic.isValid());
        Q_ASSERT(gattNotify2Characteristic.isValid());
        Q_ASSERT(gattNotify3Characteristic.isValid());
        Q_ASSERT(gattNotify4Characteristic.isValid());
        Q_ASSERT(gattNotify5Characteristic.isValid());

        // establish hook into notifications
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicChanged, this,
                &bowflext216treadmill::characteristicChanged);
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicWritten, this,
                &bowflext216treadmill::characteristicWritten);
        connect(gattCommunicationChannelService,
                static_cast<void (QLowEnergyService::*)(QLowEnergyService::ServiceError)>(&QLowEnergyService::error),
                this, &bowflext216treadmill::errorService);
        connect(gattCommunicationChannelService, &QLowEnergyService::descriptorWritten, this,
                &bowflext216treadmill::descriptorWritten);

        QByteArray descriptor;
        descriptor.append((char)0x01);
        descriptor.append((char)0x00);
        gattCommunicationChannelService->writeDescriptor(
            gattNotify1Characteristic.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
        gattCommunicationChannelService->writeDescriptor(
            gattNotify2Characteristic.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
        gattCommunicationChannelService->writeDescriptor(
            gattNotify3Characteristic.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
        gattCommunicationChannelService->writeDescriptor(
            gattNotify4Characteristic.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
        gattCommunicationChannelService->writeDescriptor(
            gattNotify5Characteristic.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
    }
}

void bowflext216treadmill::descriptorWritten(const QLowEnergyDescriptor &descriptor, const QByteArray &newValue) {
    emit debug(QStringLiteral("descriptorWritten ") + descriptor.name() + " " + newValue.toHex(' '));

    initRequest = true;
    emit connectedAndDiscovered();
}

void bowflext216treadmill::characteristicWritten(const QLowEnergyCharacteristic &characteristic,
                                                 const QByteArray &newValue) {
    Q_UNUSED(characteristic);
    emit debug(QStringLiteral("characteristicWritten ") + newValue.toHex(' '));
}

void bowflext216treadmill::serviceScanDone(void) {
    emit debug(QStringLiteral("serviceScanDone"));

    QBluetoothUuid _gattCommunicationChannelServiceId(QStringLiteral("edff9e80-cad7-11e5-ab63-0002a5d5c51b"));
    gattCommunicationChannelService = m_control->createServiceObject(_gattCommunicationChannelServiceId);
    connect(gattCommunicationChannelService, &QLowEnergyService::stateChanged, this,
            &bowflext216treadmill::stateChanged);
    gattCommunicationChannelService->discoverDetails();
}

void bowflext216treadmill::errorService(QLowEnergyService::ServiceError err) {
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceError>();
    emit debug(QStringLiteral("bowflext216treadmill::errorService ") +
               QString::fromLocal8Bit(metaEnum.valueToKey(err)) + m_control->errorString());
}

void bowflext216treadmill::error(QLowEnergyController::Error err) {
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyController::Error>();
    emit debug(QStringLiteral("bowflext216treadmill::error ") + QString::fromLocal8Bit(metaEnum.valueToKey(err)) +
               m_control->errorString());
}

void bowflext216treadmill::deviceDiscovered(const QBluetoothDeviceInfo &device) {
    emit debug(QStringLiteral("Found new device: ") + device.name() + QStringLiteral(" (") +
               device.address().toString() + ')');
    {
        bluetoothDevice = device;
        m_control = QLowEnergyController::createCentral(bluetoothDevice, this);
        connect(m_control, &QLowEnergyController::serviceDiscovered, this, &bowflext216treadmill::serviceDiscovered);
        connect(m_control, &QLowEnergyController::discoveryFinished, this, &bowflext216treadmill::serviceScanDone);
        connect(m_control,
                static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, &bowflext216treadmill::error);
        connect(m_control, &QLowEnergyController::stateChanged, this, &bowflext216treadmill::controllerStateChanged);

        connect(m_control,
                static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, [this](QLowEnergyController::Error error) {
                    Q_UNUSED(error);
                    Q_UNUSED(this);
                    emit debug(QStringLiteral("Cannot connect to remote device."));
                    emit disconnected();
                });
        connect(m_control, &QLowEnergyController::connected, this, [this]() {
            Q_UNUSED(this);
            emit debug(QStringLiteral("Controller connected. Search services..."));
            m_control->discoverServices();
        });
        connect(m_control, &QLowEnergyController::disconnected, this, [this]() {
            Q_UNUSED(this);
            emit debug(QStringLiteral("LowEnergy controller disconnected"));
            emit disconnected();
        });

        // Connect
        m_control->connectToDevice();
        return;
    }
}

void bowflext216treadmill::controllerStateChanged(QLowEnergyController::ControllerState state) {
    qDebug() << QStringLiteral("controllerStateChanged") << state;
    if (state == QLowEnergyController::UnconnectedState && m_control) {
        qDebug() << QStringLiteral("trying to connect back again...");
        initDone = false;
        m_control->connectToDevice();
    }
}

bool bowflext216treadmill::connected() {
    if (!m_control) {
        return false;
    }
    return m_control->state() == QLowEnergyController::DiscoveredState;
}

void *bowflext216treadmill::VirtualTreadMill() { return virtualTreadMill; }

void *bowflext216treadmill::VirtualDevice() { return VirtualTreadMill(); }

bool bowflext216treadmill::autoPauseWhenSpeedIsZero() {
    if (lastStart == 0 || QDateTime::currentMSecsSinceEpoch() > (lastStart + 10000))
        return true;
    else
        return false;
}

bool bowflext216treadmill::autoStartWhenSpeedIsGreaterThenZero() {
    if ((lastStop == 0 || QDateTime::currentMSecsSinceEpoch() > (lastStop + 25000)) && requestStop == -1)
        return true;
    else
        return false;
}