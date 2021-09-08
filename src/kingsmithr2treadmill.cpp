#include "kingsmithr2treadmill.h"
#include "ios/lockscreen.h"
#include "keepawakehelper.h"
#include "virtualtreadmill.h"
#include <QBluetoothLocalDevice>
#include <QDateTime>
#include <QFile>
#include <QMetaEnum>
#include <QSettings>
#include <chrono>

using namespace std::chrono_literals;

kingsmithr2treadmill::kingsmithr2treadmill(uint32_t pollDeviceTime, bool noConsole, bool noHeartService,
                                                 double forceInitSpeed, double forceInitInclination) {
    m_watt.setType(metric::METRIC_WATT);
    Speed.setType(metric::METRIC_SPEED);
    this->noConsole = noConsole;
    this->noHeartService = noHeartService;

    if (forceInitSpeed > 0) {
        lastSpeed = forceInitSpeed;
    }

    if (forceInitInclination > 0) {
        lastInclination = forceInitInclination;
    }

    refresh = new QTimer(this);
    initDone = false;
    connect(refresh, &QTimer::timeout, this, &kingsmithr2treadmill::update);
    refresh->start(pollDeviceTime);
}

void kingsmithr2treadmill::writeCharacteristic(const QString &data, const QString &info,
                                               bool disable_log, bool wait_for_response) {
    QEventLoop loop;
    QTimer timeout;

    if (wait_for_response) {
        connect(this, &kingsmithr2treadmill::packetReceived, &loop, &QEventLoop::quit);
        timeout.singleShot(300ms, &loop, &QEventLoop::quit);
    } else {
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicWritten, &loop, &QEventLoop::quit);
        timeout.singleShot(300ms, &loop, &QEventLoop::quit);
    }

    if (gattCommunicationChannelService->state() != QLowEnergyService::ServiceState::ServiceDiscovered ||
        m_control->state() == QLowEnergyController::UnconnectedState) {
        emit debug(QStringLiteral("writeCharacteristic error because the connection is closed"));

        return;
    }

    QByteArray input = data.toUtf8().toBase64();
    QByteArray encrypted;
    for (int i = 0; i < input.length(); i++) {
        int idx = PLAINTEXT_TABLE.indexOf(input.at(i));
        encrypted.append(ENCRYPT_TABLE[idx]);
    }
    if (!disable_log) {
        emit debug(QStringLiteral(" >> plain: ") + data + QStringLiteral(" // ") + info);
        emit debug(QStringLiteral(" >> base64: ") + QString(input) + QStringLiteral(" // ") + info);
        emit debug(QStringLiteral(" >> encrypted: ") + QString(encrypted) + QStringLiteral(" // ") + info);
    }
    encrypted.append('\x0d');
    for (int i = 0; i < encrypted.length(); i+=16) {
        gattCommunicationChannelService->writeCharacteristic(
            gattWriteCharacteristic, encrypted.mid(i, 16), QLowEnergyService::WriteWithoutResponse);
    }

    if (!disable_log) {
        emit debug(QStringLiteral(" >> ") + encrypted.toHex(' ') +
                   QStringLiteral(" // ") + info);
    }

    loop.exec();

    if (timeout.isActive() == false) {
        emit debug(QStringLiteral(" exit for timeout"));
    }
}

void kingsmithr2treadmill::updateDisplay(uint16_t elapsed) {}

void kingsmithr2treadmill::forceSpeedOrIncline(double requestSpeed, double requestIncline) {
    Q_UNUSED(requestIncline)
    QString speed = QString::number(requestSpeed);
    writeCharacteristic(
        QStringLiteral("props CurrentSpeed ") + speed, QStringLiteral("forceSpeed") + speed, false, true);
}

bool kingsmithr2treadmill::sendChangeFanSpeed(uint8_t speed) { return false; }

bool kingsmithr2treadmill::changeFanSpeed(uint8_t speed) {

    requestFanSpeed = speed;

    return false;
}

void kingsmithr2treadmill::changeInclinationRequested(double grade, double percentage) {
    Q_UNUSED(grade);
    if (percentage < 0)
        percentage = 0;
    changeInclination(percentage);
}

void kingsmithr2treadmill::update() {
    if (m_control->state() == QLowEnergyController::UnconnectedState) {
        emit disconnected();
        return;
    }

    if (initRequest) {

        initRequest = false;
        btinit((lastSpeed > 0 ? true : false));
    } else if (/*bluetoothDevice.isValid() &&*/
               m_control->state() == QLowEnergyController::DiscoveredState && gattCommunicationChannelService &&
               gattWriteCharacteristic.isValid() && gattNotifyCharacteristic.isValid() && initDone) {

        QSettings settings;
        // ******************************************* virtual treadmill init *************************************
        if (!firstInit && searchStopped && !virtualTreadMill && !virtualBike) {
            bool virtual_device_enabled = settings.value("virtual_device_enabled", true).toBool();
            bool virtual_device_force_bike = settings.value("virtual_device_force_bike", false).toBool();
            if (virtual_device_enabled) {
                if (!virtual_device_force_bike) {
                    debug("creating virtual treadmill interface...");
                    virtualTreadMill = new virtualtreadmill(this, noHeartService);
                    connect(virtualTreadMill, &virtualtreadmill::debug, this, &kingsmithr2treadmill::debug);
                } else {
                    debug("creating virtual bike interface...");
                    virtualBike = new virtualbike(this);
                    connect(virtualBike, &virtualbike::changeInclination, this,
                            &kingsmithr2treadmill::changeInclinationRequested);
                }
                firstInit = 1;
            }
        }
        // ********************************************************************************************************

        // debug("Domyos Treadmill RSSI " + QString::number(bluetoothDevice.rssi()));

        update_metrics(true, watts(settings.value(QStringLiteral("weight"), 75.0).toFloat()));

        // updating the treadmill console every second
        if (sec1Update++ >= (1000 / refresh->interval())) {

            sec1Update = 0;
            updateDisplay(elapsed.value());
        } else {
            //writeCharacteristic(QStringLiteral(""), QStringLiteral("noOp"), false, true);
        }

        // byte 3 - 4 = elapsed time
        // byte 17    = inclination
        if (requestSpeed != -1) {
            if (requestSpeed != currentSpeed().value() && requestSpeed >= 0 && requestSpeed <= 22) {
                emit debug(QStringLiteral("writing speed ") + QString::number(requestSpeed));

                double inc = Inclination.value();
                if (requestInclination != -1) {

                    // only 0.5 steps ara avaiable
                    requestInclination = qRound(requestInclination * 2.0) / 2.0;
                    inc = requestInclination;
                    requestInclination = -1;
                }
                forceSpeedOrIncline(requestSpeed, inc);
            }
            requestSpeed = -1;
        }
        if (requestInclination != -1) {
            // only 0.5 steps ara avaiable
            requestInclination = qRound(requestInclination * 2.0) / 2.0;
            if (requestInclination != currentInclination().value() && requestInclination >= 0 &&
                requestInclination <= 15) {
                emit debug(QStringLiteral("writing incline ") + QString::number(requestInclination));

                double speed = currentSpeed().value();
                if (requestSpeed != -1) {

                    speed = requestSpeed;
                    requestSpeed = -1;
                }
                forceSpeedOrIncline(speed, requestInclination);
            }
            requestInclination = -1;
        }
        if (requestStart != -1) {
            emit debug(QStringLiteral("starting..."));
            if (lastSpeed == 0.0) {

                lastSpeed = 0.5;
            }
            // btinit(true);
            requestStart = -1;
            emit tapeStarted();
        }
        if (requestStop != -1) {
            emit debug(QStringLiteral("stopping..."));
            requestStop = -1;
        }
        if (requestFanSpeed != -1) {
            emit debug(QStringLiteral("changing fan speed..."));

            sendChangeFanSpeed(requestFanSpeed);
            requestFanSpeed = -1;
        }
        if (requestIncreaseFan != -1) {
            emit debug(QStringLiteral("increasing fan speed..."));

            sendChangeFanSpeed(FanSpeed + 1);
            requestIncreaseFan = -1;
        } else if (requestDecreaseFan != -1) {
            emit debug(QStringLiteral("decreasing fan speed..."));

            sendChangeFanSpeed(FanSpeed - 1);
            requestDecreaseFan = -1;
        }
    }
}

void kingsmithr2treadmill::serviceDiscovered(const QBluetoothUuid &gatt) {
    emit debug(QStringLiteral("serviceDiscovered ") + gatt.toString());
}

void kingsmithr2treadmill::characteristicChanged(const QLowEnergyCharacteristic &characteristic,
                                                    const QByteArray &newValue) {
    // qDebug() << "characteristicChanged" << characteristic.uuid() << newValue << newValue.length();
    QSettings settings;
    QString heartRateBeltName =
        settings.value(QStringLiteral("heart_rate_belt_name"), QStringLiteral("Disabled")).toString();
    Q_UNUSED(characteristic);
    QByteArray value = newValue;

    emit debug(QStringLiteral(" << ") + QString::number(value.length()) + QStringLiteral(" ") + value.toHex(' '));

    buffer.append(value);
    if (value.back() != '\x0d') {
        emit debug(QStringLiteral("packet not finished"));
        return;
    }
    QByteArray decrypted;
    for (int i = 0; i < buffer.length(); i++) {
        char ch = buffer.at(i);
        if (ch == '\x0d') {
            continue;
        }
        int idx = ENCRYPT_TABLE.indexOf(ch);
        decrypted.append(PLAINTEXT_TABLE[idx]);
    }
    buffer.clear();
    lastValue = QByteArray::fromBase64(decrypted);

    emit packetReceived();

    QString data = QString(lastValue);
    emit debug(QStringLiteral(" << decrypted: ") + QString(data));

    if (!data.startsWith("props")) {
        // TODO
        return;
    }

    QList _props = data.split(QStringLiteral(" "), QString::SkipEmptyParts);
    for (int i = 1; i < _props.size(); i += 2) {
        QString key = _props.at(i);
        // skip string params
        if (!key.compare(QStringLiteral("mcu_version")) ||
            !key.compare(QStringLiteral("goal"))) {
            continue;
        }
        QString value = _props.at(i + 1);
        emit debug(key + ": " + value);
        props[key] = value.toDouble();
    }

    double speed = props.value("CurrentSpeed", 0);
    // TODO:
    // - RunningDistance (int; meter) : update each 10miters / 0.01 mile
    // - RunningSteps (int) : update 2 steps
    // - BurnCalories (int) : KCal * 1000
    // - RunningTotalTime (int; sec)
    // - spm (int) : steps per minute


#ifdef Q_OS_ANDROID
    if (settings.value("ant_heart", false).toBool())
        Heart = (uint8_t)KeepAwakeHelper::heart();
    else
#endif
    {
        if (heartRateBeltName.startsWith(QStringLiteral("Disabled"))) {

            uint8_t heart = 0;
            if (heart == 0) {

#ifdef Q_OS_IOS
#ifndef IO_UNDER_QT
                lockscreen h;
                long appleWatchHeartRate = h.heartRate();
                h.setKcal(KCal.value());
                h.setDistance(Distance.value());
                Heart = appleWatchHeartRate;
                debug("Current Heart from Apple Watch: " + QString::number(appleWatchHeartRate));
#endif
#endif
            } else

                Heart = heart;
        }
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

        DistanceCalculated +=
            ((speed / (double)3600.0) /
             ((double)1000.0 / (double)(lastTimeCharacteristicChanged.msecsTo(QDateTime::currentDateTime()))));
        lastTimeCharacteristicChanged = QDateTime::currentDateTime();
    }

    emit debug(QStringLiteral("Current speed: ") + QString::number(speed));
    // emit debug(QStringLiteral("Current incline: ") + QString::number(incline));
    emit debug(QStringLiteral("Current heart: ") + QString::number(Heart.value()));
    emit debug(QStringLiteral("Current KCal: ") + QString::number(KCal.value()));
    // emit debug(QStringLiteral("Current Distance: ") + QString::number(distance));
    emit debug(QStringLiteral("Current Distance Calculated: ") + QString::number(DistanceCalculated));

    if (m_control->error() != QLowEnergyController::NoError) {
        qDebug() << QStringLiteral("QLowEnergyController ERROR!!") << m_control->errorString();
    }

    if (Speed.value() != speed) {

        emit speedChanged(speed);
    }
    Speed = speed;
    Distance = DistanceCalculated;

    if (speed > 0) {

        lastSpeed = speed;
        // lastInclination = incline;
    }

    firstCharacteristicChanged = false;
}

void kingsmithr2treadmill::btinit(bool startTape) {
    emit debug(QStringLiteral("btinit"));

    writeCharacteristic(QStringLiteral(""), QStringLiteral("init"), false, true);
    // format error
    writeCharacteristic(QStringLiteral("shake"), QStringLiteral("init"), false, true);
    // shake 00
    writeCharacteristic(QStringLiteral("net"), QStringLiteral("init"), false, true);
    // net cloud
    writeCharacteristic(QStringLiteral("get_dn"), QStringLiteral("init"), false, true);
    // get_dn XXXX...
    writeCharacteristic(QStringLiteral("get_pk"), QStringLiteral("init"), false, true);
    // get_pk XXXX...
    uint64_t timestamp = QDateTime::currentSecsSinceEpoch();
    writeCharacteristic(QStringLiteral("time_posix %1").arg(timestamp), QStringLiteral("init"), false, true);
    // time_posix 0
    writeCharacteristic(QStringLiteral("version"), QStringLiteral("init"), false, true);
    // version 0014

    // read current properties
    //writeCharacteristic(
    //    QStringLiteral("servers getProp 1 3 7 8 9 16 17 18 19 21 22 23 24 31"), QStringLiteral("init"), false, true);
    writeCharacteristic(
        QStringLiteral("servers getProp 1 2 7 12 23 24 31"), QStringLiteral("init"), false, true);

    // TODO need reset BurnCalories & RunningDistance
    initDone = true;
}

void kingsmithr2treadmill::stateChanged(QLowEnergyService::ServiceState state) {

    QBluetoothUuid _gattWriteCharacteristicId((quint16)0xFED7);
    QBluetoothUuid _gattNotifyCharacteristicId((quint16)0xFED8);
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceState>();
    emit debug(QStringLiteral("BTLE stateChanged ") + QString::fromLocal8Bit(metaEnum.valueToKey(state)));
    if (state == QLowEnergyService::DiscoveringServices) {
    }
    if (state == QLowEnergyService::ServiceDiscovered) {

        // qDebug() << gattCommunicationChannelService->characteristics();

        gattWriteCharacteristic = gattCommunicationChannelService->characteristic(_gattWriteCharacteristicId);
        gattNotifyCharacteristic = gattCommunicationChannelService->characteristic(_gattNotifyCharacteristicId);
        Q_ASSERT(gattWriteCharacteristic.isValid());
        Q_ASSERT(gattNotifyCharacteristic.isValid());

        // establish hook into notifications
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicChanged, this,
                &kingsmithr2treadmill::characteristicChanged);
        connect(gattCommunicationChannelService, &QLowEnergyService::characteristicWritten, this,
                &kingsmithr2treadmill::characteristicWritten);
        connect(gattCommunicationChannelService,
                static_cast<void (QLowEnergyService::*)(QLowEnergyService::ServiceError)>(&QLowEnergyService::error),
                this, &kingsmithr2treadmill::errorService);
        connect(gattCommunicationChannelService, &QLowEnergyService::descriptorWritten, this,
                &kingsmithr2treadmill::descriptorWritten);

        QByteArray descriptor;
        descriptor.append((char)0x01);
        descriptor.append((char)0x00);
        gattCommunicationChannelService->writeDescriptor(
            gattNotifyCharacteristic.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
    }
}

void kingsmithr2treadmill::descriptorWritten(const QLowEnergyDescriptor &descriptor, const QByteArray &newValue) {
    emit debug(QStringLiteral("descriptorWritten ") + descriptor.name() + QStringLiteral(" ") + newValue.toHex(' '));

    initRequest = true;
    emit connectedAndDiscovered();
}

void kingsmithr2treadmill::characteristicWritten(const QLowEnergyCharacteristic &characteristic,
                                                    const QByteArray &newValue) {
    Q_UNUSED(characteristic);
    emit debug(QStringLiteral("characteristicWritten ") + newValue.toHex(' '));
}

void kingsmithr2treadmill::serviceScanDone(void) {
    QBluetoothUuid _gattCommunicationChannelServiceId((quint16)0x1234);
    emit debug(QStringLiteral("serviceScanDone"));

    gattCommunicationChannelService = m_control->createServiceObject(_gattCommunicationChannelServiceId);
    connect(gattCommunicationChannelService, &QLowEnergyService::stateChanged, this,
            &kingsmithr2treadmill::stateChanged);
    gattCommunicationChannelService->discoverDetails();
}

void kingsmithr2treadmill::errorService(QLowEnergyService::ServiceError err) {

    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceError>();
    emit debug(QStringLiteral("kingsmithr2treadmill::errorService ") +
               QString::fromLocal8Bit(metaEnum.valueToKey(err)) + m_control->errorString());
}

void kingsmithr2treadmill::error(QLowEnergyController::Error err) {

    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyController::Error>();
    emit debug(QStringLiteral("kingsmithr2treadmill::error ") + QString::fromLocal8Bit(metaEnum.valueToKey(err)) +
               m_control->errorString());
}

void kingsmithr2treadmill::deviceDiscovered(const QBluetoothDeviceInfo &device) {
    {

        bluetoothDevice = device;
        m_control = QLowEnergyController::createCentral(bluetoothDevice, this);
        connect(m_control, &QLowEnergyController::serviceDiscovered, this, &kingsmithr2treadmill::serviceDiscovered);
        connect(m_control, &QLowEnergyController::discoveryFinished, this, &kingsmithr2treadmill::serviceScanDone);
        connect(m_control,
                static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, &kingsmithr2treadmill::error);
        connect(m_control, &QLowEnergyController::stateChanged, this, &kingsmithr2treadmill::controllerStateChanged);

        connect(m_control,
                static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, [this](QLowEnergyController::Error error) {
                    Q_UNUSED(error);
                    Q_UNUSED(this);
                    emit debug(QStringLiteral("Cannot connect to remote device."));
                    searchStopped = false;
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
            searchStopped = false;
            emit disconnected();
        });

        // Connect
        m_control->connectToDevice();
        return;
    }
}

void kingsmithr2treadmill::controllerStateChanged(QLowEnergyController::ControllerState state) {
    qDebug() << QStringLiteral("controllerStateChanged") << state;
    if (state == QLowEnergyController::UnconnectedState && m_control) {
        qDebug() << QStringLiteral("trying to connect back again...");

        initDone = false;
        m_control->connectToDevice();
    }
}

bool kingsmithr2treadmill::connected() {
    if (!m_control) {

        return false;
    }
    return m_control->state() == QLowEnergyController::DiscoveredState;
}

void *kingsmithr2treadmill::VirtualTreadMill() { return virtualTreadMill; }

void *kingsmithr2treadmill::VirtualDevice() { return VirtualTreadMill(); }

double kingsmithr2treadmill::odometer() { return DistanceCalculated; }

void kingsmithr2treadmill::setLastSpeed(double speed) { lastSpeed = speed; }

void kingsmithr2treadmill::setLastInclination(double inclination) { lastInclination = inclination; }

void kingsmithr2treadmill::searchingStop() { searchStopped = true; }