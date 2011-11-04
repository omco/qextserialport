#include "qextserialport.h"
#include "qextserialport_p.h"
#include <QMutexLocker>
#include <QDebug>
#include <QRegExp>
#include <QMetaType>

void QextSerialPortPrivate::platformSpecificInit()
{
    Win_Handle=INVALID_HANDLE_VALUE;
    ZeroMemory(&overlap, sizeof(OVERLAPPED));
    overlap.hEvent = CreateEvent(NULL, true, false, NULL);
    winEventNotifier = 0;
    bytesToWriteLock = new QReadWriteLock;
    _bytesToWrite = 0;
}

void QextSerialPortPrivate::platformSpecificDestruct() {
    CloseHandle(overlap.hEvent);
    delete bytesToWriteLock;
}

QString QextSerialPort::fullPortNameWin(const QString & name)
{
    QRegExp rx("^COM(\\d+)");
    QString fullName(name);
    if(fullName.contains(rx)) {
        int portnum = rx.cap(1).toInt();
        if(portnum > 9) // COM ports greater than 9 need \\.\ prepended
            fullName.prepend("\\\\.\\");
    }
    return fullName;
}

bool QextSerialPortPrivate::open_sys(QIODevice::OpenMode mode)
{
    Q_D(QextSerialPort)
    unsigned long confSize = sizeof(COMMCONFIG);
    d->Win_CommConfig.dwSize = confSize;
    DWORD dwFlagsAndAttributes = 0;
    if (queryMode() == QextSerialPort::EventDriven)
        dwFlagsAndAttributes |= FILE_FLAG_OVERLAPPED;

    QMutexLocker lock(mutex);
    /*open the port*/
    Win_Handle=CreateFileA(port.toAscii(), GENERIC_READ|GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, dwFlagsAndAttributes, NULL);
    if (Win_Handle!=INVALID_HANDLE_VALUE) {
        QIODevice::open(mode);
        /*configure port settings*/
        GetCommConfig(Win_Handle, &Win_CommConfig, &confSize);
        GetCommState(Win_Handle, &(Win_CommConfig.dcb));

        /*set up parameters*/
        Win_CommConfig.dcb.fBinary=TRUE;
        Win_CommConfig.dcb.fInX=FALSE;
        Win_CommConfig.dcb.fOutX=FALSE;
        Win_CommConfig.dcb.fAbortOnError=FALSE;
        Win_CommConfig.dcb.fNull=FALSE;
        setBaudRate(Settings.BaudRate);
        setDataBits(Settings.DataBits);
        setStopBits(Settings.StopBits);
        setParity(Settings.Parity);
        setFlowControl(Settings.FlowControl);
        setTimeout(Settings.Timeout_Millisec);
        SetCommConfig(Win_Handle, &Win_CommConfig, sizeof(COMMCONFIG));

        //init event driven approach
        if (queryMode() == QextSerialPort::EventDriven) {
            Win_CommTimeouts.ReadIntervalTimeout = MAXDWORD;
            Win_CommTimeouts.ReadTotalTimeoutMultiplier = 0;
            Win_CommTimeouts.ReadTotalTimeoutConstant = 0;
            Win_CommTimeouts.WriteTotalTimeoutMultiplier = 0;
            Win_CommTimeouts.WriteTotalTimeoutConstant = 0;
            SetCommTimeouts(Win_Handle, &Win_CommTimeouts);
            if (!SetCommMask( Win_Handle, EV_TXEMPTY | EV_RXCHAR | EV_DSR)) {
                qWarning() << "failed to set Comm Mask. Error code:", GetLastError();
                return false;
            }
            winEventNotifier = new QWinEventNotifier(overlap.hEvent);
            qRegisterMetaType<HANDLE>("HANDLE");
            connect(winEventNotifier, SIGNAL(activated(HANDLE)), this, SLOT(onWinEvent(HANDLE)), Qt::DirectConnection);
            WaitCommEvent(Win_Handle, &eventMask, &overlap);
        }
        return true;
    }
    return false;
}

void QextSerialPortPrivate::close_sys()
{
    QMutexLocker lock(mutex);
    flush_sys();
    QIODevice::close(); // mark ourselves as closed
    CancelIo(Win_Handle);
    if (CloseHandle(Win_Handle))
        Win_Handle = INVALID_HANDLE_VALUE;
    if (winEventNotifier){
        winEventNotifier->deleteLater();
        winEventNotifier = 0;
    }
    _bytesToWrite = 0;

    foreach(OVERLAPPED* o, pendingWrites) {
        CloseHandle(o->hEvent);
        delete o;
    }
    pendingWrites.clear();
}

void QextSerialPortPrivate::flush_sys()
{
    QMutexLocker lock(mutex);
    FlushFileBuffers(Win_Handle);
}

/*!
    This function will return the number of bytes waiting in the receive queue of the serial port.
    It is included primarily to provide a complete QIODevice interface, and will not record errors
    in the lastErr member (because it is const).  This function is also not thread-safe - in
    multithreading situations, use QextSerialPort::bytesAvailable() instead.
*/
qint64 QextSerialPort::size() const {
    int availBytes;
    COMSTAT Win_ComStat;
    DWORD Win_ErrorMask=0;
    ClearCommError(Win_Handle, &Win_ErrorMask, &Win_ComStat);
    availBytes = Win_ComStat.cbInQue;
    return (qint64)availBytes;
}

/*!
    Returns the number of bytes waiting in the port's receive queue.  This function will return 0 if
    the port is not currently open, or -1 on error.
*/
qint64 QextSerialPort::bytesAvailable() const {
    QMutexLocker lock(mutex);
    if (isOpen()) {
        DWORD Errors;
        COMSTAT Status;
        if (ClearCommError(Win_Handle, &Errors, &Status)) {
            return Status.cbInQue + QIODevice::bytesAvailable();
        }
        return (qint64)-1;
    }
    return 0;
}

/*!
    Translates a system-specific error code to a QextSerialPort error code.  Used internally.
*/
void QextSerialPortPrivate::translateError(ulong error) {
    if (error&CE_BREAK) {
        lastErr=E_BREAK_CONDITION;
    }
    else if (error&CE_FRAME) {
        lastErr=E_FRAMING_ERROR;
    }
    else if (error&CE_IOE) {
        lastErr=E_IO_ERROR;
    }
    else if (error&CE_MODE) {
        lastErr=E_INVALID_FD;
    }
    else if (error&CE_OVERRUN) {
        lastErr=E_BUFFER_OVERRUN;
    }
    else if (error&CE_RXPARITY) {
        lastErr=E_RECEIVE_PARITY_ERROR;
    }
    else if (error&CE_RXOVER) {
        lastErr=E_RECEIVE_OVERFLOW;
    }
    else if (error&CE_TXFULL) {
        lastErr=E_TRANSMIT_OVERFLOW;
    }
}

/*!
    Reads a block of data from the serial port.  This function will read at most maxlen bytes from
    the serial port and place them in the buffer pointed to by data.  Return value is the number of
    bytes actually read, or -1 on error.
    
    \warning before calling this function ensure that serial port associated with this class
    is currently open (use isOpen() function to check if port is open).
*/
qint64 QextSerialPort::readData(char *data, qint64 maxSize)
{
    DWORD retVal;
    QMutexLocker lock(mutex);
    retVal = 0;
    if (queryMode() == QextSerialPort::EventDriven) {
        OVERLAPPED overlapRead;
        ZeroMemory(&overlapRead, sizeof(OVERLAPPED));
        if (!ReadFile(Win_Handle, (void*)data, (DWORD)maxSize, & retVal, & overlapRead)) {
            if (GetLastError() == ERROR_IO_PENDING)
                GetOverlappedResult(Win_Handle, & overlapRead, & retVal, true);
            else {
                lastErr = E_READ_FAILED;
                retVal = (DWORD)-1;
            }
        }
    } else if (!ReadFile(Win_Handle, (void*)data, (DWORD)maxSize, & retVal, NULL)) {
        lastErr = E_READ_FAILED;
        retVal = (DWORD)-1;
    }
    return (qint64)retVal;
}

/*!
    Writes a block of data to the serial port.  This function will write len bytes
    from the buffer pointed to by data to the serial port.  Return value is the number
    of bytes actually written, or -1 on error.
    
    \warning before calling this function ensure that serial port associated with this class
    is currently open (use isOpen() function to check if port is open).
*/
qint64 QextSerialPort::writeData(const char *data, qint64 maxSize)
{
    QMutexLocker lock( mutex );
    DWORD retVal = 0;
    if (queryMode() == QextSerialPort::EventDriven) {
        OVERLAPPED* newOverlapWrite = new OVERLAPPED;
        ZeroMemory(newOverlapWrite, sizeof(OVERLAPPED));
        newOverlapWrite->hEvent = CreateEvent(NULL, true, false, NULL);
        if (WriteFile(Win_Handle, (void*)data, (DWORD)maxSize, & retVal, newOverlapWrite)) {
            CloseHandle(newOverlapWrite->hEvent);
            delete newOverlapWrite;
        }
        else if (GetLastError() == ERROR_IO_PENDING) {
            // writing asynchronously...not an error
            QWriteLocker writelocker(bytesToWriteLock);
            _bytesToWrite += maxSize;
            pendingWrites.append(newOverlapWrite);
        }
        else {
            qDebug() << "serialport write error:" << GetLastError();
            lastErr = E_WRITE_FAILED;
            retVal = (DWORD)-1;
            if(!CancelIo(newOverlapWrite->hEvent))
                qDebug() << "serialport: couldn't cancel IO";
            if(!CloseHandle(newOverlapWrite->hEvent))
                qDebug() << "serialport: couldn't close OVERLAPPED handle";
            delete newOverlapWrite;
        }
    } else if (!WriteFile(Win_Handle, (void*)data, (DWORD)maxSize, & retVal, NULL)) {
        lastErr = E_WRITE_FAILED;
        retVal = (DWORD)-1;
    }
    return (qint64)retVal;
}

void QextSerialPortPrivate::setFlowControl(FlowType flow) {
    QMutexLocker lock(mutex);
    if (Settings.FlowControl!=flow) {
        Settings.FlowControl=flow;
    }
    if (isOpen()) {
        switch(flow) {

            /*no flow control*/
            case FLOW_OFF:
                Win_CommConfig.dcb.fOutxCtsFlow=FALSE;
                Win_CommConfig.dcb.fRtsControl=RTS_CONTROL_DISABLE;
                Win_CommConfig.dcb.fInX=FALSE;
                Win_CommConfig.dcb.fOutX=FALSE;
                SetCommConfig(Win_Handle, &Win_CommConfig, sizeof(COMMCONFIG));
                break;

            /*software (XON/XOFF) flow control*/
            case FLOW_XONXOFF:
                Win_CommConfig.dcb.fOutxCtsFlow=FALSE;
                Win_CommConfig.dcb.fRtsControl=RTS_CONTROL_DISABLE;
                Win_CommConfig.dcb.fInX=TRUE;
                Win_CommConfig.dcb.fOutX=TRUE;
                SetCommConfig(Win_Handle, &Win_CommConfig, sizeof(COMMCONFIG));
                break;

            case FLOW_HARDWARE:
                Win_CommConfig.dcb.fOutxCtsFlow=TRUE;
                Win_CommConfig.dcb.fRtsControl=RTS_CONTROL_HANDSHAKE;
                Win_CommConfig.dcb.fInX=FALSE;
                Win_CommConfig.dcb.fOutX=FALSE;
                SetCommConfig(Win_Handle, &Win_CommConfig, sizeof(COMMCONFIG));
                break;
        }
    }
}

void QextSerialPortPrivate::setParity(ParityType parity) {
    QMutexLocker lock(mutex);
    if (Settings.Parity!=parity) {
        Settings.Parity=parity;
    }
    if (isOpen()) {
        Win_CommConfig.dcb.Parity=(unsigned char)parity;
        switch (parity) {

            /*space parity*/
            case PAR_SPACE:
                if (Settings.DataBits==DATA_8) {
                    TTY_PORTABILITY_WARNING("QextSerialPort Portability Warning: Space parity with 8 data bits is not supported by POSIX systems.");
                }
                Win_CommConfig.dcb.fParity=TRUE;
                break;

            /*mark parity - WINDOWS ONLY*/
            case PAR_MARK:
                TTY_PORTABILITY_WARNING("QextSerialPort Portability Warning:  Mark parity is not supported by POSIX systems");
                Win_CommConfig.dcb.fParity=TRUE;
                break;

            /*no parity*/
            case PAR_NONE:
                Win_CommConfig.dcb.fParity=FALSE;
                break;

            /*even parity*/
            case PAR_EVEN:
                Win_CommConfig.dcb.fParity=TRUE;
                break;

            /*odd parity*/
            case PAR_ODD:
                Win_CommConfig.dcb.fParity=TRUE;
                break;
        }
        SetCommConfig(Win_Handle, &Win_CommConfig, sizeof(COMMCONFIG));
    }
}

void QextSerialPortPrivate::setDataBits(DataBitsType dataBits) {
    QMutexLocker lock(mutex);
    if (Settings.DataBits!=dataBits) {
        if ((Settings.StopBits==STOP_2 && dataBits==DATA_5) ||
            (Settings.StopBits==STOP_1_5 && dataBits!=DATA_5)) {
        }
        else {
            Settings.DataBits=dataBits;
        }
    }
    if (isOpen()) {
        switch(dataBits) {

            /*5 data bits*/
            case DATA_5:
                if (Settings.StopBits==STOP_2) {
                    TTY_WARNING("QextSerialPort: 5 Data bits cannot be used with 2 stop bits.");
                }
                else {
                    Win_CommConfig.dcb.ByteSize=5;
                    SetCommConfig(Win_Handle, &Win_CommConfig, sizeof(COMMCONFIG));
                }
                break;

            /*6 data bits*/
            case DATA_6:
                if (Settings.StopBits==STOP_1_5) {
                    TTY_WARNING("QextSerialPort: 6 Data bits cannot be used with 1.5 stop bits.");
                }
                else {
                    Win_CommConfig.dcb.ByteSize=6;
                    SetCommConfig(Win_Handle, &Win_CommConfig, sizeof(COMMCONFIG));
                }
                break;

            /*7 data bits*/
            case DATA_7:
                if (Settings.StopBits==STOP_1_5) {
                    TTY_WARNING("QextSerialPort: 7 Data bits cannot be used with 1.5 stop bits.");
                }
                else {
                    Win_CommConfig.dcb.ByteSize=7;
                    SetCommConfig(Win_Handle, &Win_CommConfig, sizeof(COMMCONFIG));
                }
                break;

            /*8 data bits*/
            case DATA_8:
                if (Settings.StopBits==STOP_1_5) {
                    TTY_WARNING("QextSerialPort: 8 Data bits cannot be used with 1.5 stop bits.");
                }
                else {
                    Win_CommConfig.dcb.ByteSize=8;
                    SetCommConfig(Win_Handle, &Win_CommConfig, sizeof(COMMCONFIG));
                }
                break;
        }
    }
}

void QextSerialPortPrivate::setStopBits(StopBitsType stopBits) {
    QMutexLocker lock(mutex);
    if (Settings.StopBits!=stopBits) {
        if ((Settings.DataBits==DATA_5 && stopBits==STOP_2) ||
            (stopBits==STOP_1_5 && Settings.DataBits!=DATA_5)) {
        }
        else {
            Settings.StopBits=stopBits;
        }
    }
    if (isOpen()) {
        switch (stopBits) {

            /*one stop bit*/
            case STOP_1:
                Win_CommConfig.dcb.StopBits=ONESTOPBIT;
                SetCommConfig(Win_Handle, &Win_CommConfig, sizeof(COMMCONFIG));
                break;

            /*1.5 stop bits*/
            case STOP_1_5:
                TTY_PORTABILITY_WARNING("QextSerialPort Portability Warning: 1.5 stop bit operation is not supported by POSIX.");
                if (Settings.DataBits!=DATA_5) {
                    TTY_WARNING("QextSerialPort: 1.5 stop bits can only be used with 5 data bits");
                }
                else {
                    Win_CommConfig.dcb.StopBits=ONE5STOPBITS;
                    SetCommConfig(Win_Handle, &Win_CommConfig, sizeof(COMMCONFIG));
                }
                break;

            /*two stop bits*/
            case STOP_2:
                if (Settings.DataBits==DATA_5) {
                    TTY_WARNING("QextSerialPort: 2 stop bits cannot be used with 5 data bits");
                }
                else {
                    Win_CommConfig.dcb.StopBits=TWOSTOPBITS;
                    SetCommConfig(Win_Handle, &Win_CommConfig, sizeof(COMMCONFIG));
                }
                break;
        }
    }
}

void QextSerialPortPrivate::setBaudRate(BaudRateType baudRate) {
    QMutexLocker lock(mutex);
    if (Settings.BaudRate!=baudRate) {
        switch (baudRate) {
            case BAUD50:
            case BAUD75:
            case BAUD134:
            case BAUD150:
            case BAUD200:
                Settings.BaudRate=BAUD110;
                break;

            case BAUD1800:
                Settings.BaudRate=BAUD1200;
                break;

            case BAUD76800:
                Settings.BaudRate=BAUD57600;
                break;

            default:
                Settings.BaudRate=baudRate;
                break;
        }
    }
    if (isOpen()) {
        switch (baudRate) {

            /*50 baud*/
            case BAUD50:
                TTY_WARNING("QextSerialPort: Windows does not support 50 baud operation.  Switching to 110 baud.");
                Win_CommConfig.dcb.BaudRate=CBR_110;
                break;

            /*75 baud*/
            case BAUD75:
                TTY_WARNING("QextSerialPort: Windows does not support 75 baud operation.  Switching to 110 baud.");
                Win_CommConfig.dcb.BaudRate=CBR_110;
                break;

            /*110 baud*/
            case BAUD110:
                Win_CommConfig.dcb.BaudRate=CBR_110;
                break;

            /*134.5 baud*/
            case BAUD134:
                TTY_WARNING("QextSerialPort: Windows does not support 134.5 baud operation.  Switching to 110 baud.");
                Win_CommConfig.dcb.BaudRate=CBR_110;
                break;

            /*150 baud*/
            case BAUD150:
                TTY_WARNING("QextSerialPort: Windows does not support 150 baud operation.  Switching to 110 baud.");
                Win_CommConfig.dcb.BaudRate=CBR_110;
                break;

            /*200 baud*/
            case BAUD200:
                TTY_WARNING("QextSerialPort: Windows does not support 200 baud operation.  Switching to 110 baud.");
                Win_CommConfig.dcb.BaudRate=CBR_110;
                break;

            /*300 baud*/
            case BAUD300:
                Win_CommConfig.dcb.BaudRate=CBR_300;
                break;

            /*600 baud*/
            case BAUD600:
                Win_CommConfig.dcb.BaudRate=CBR_600;
                break;

            /*1200 baud*/
            case BAUD1200:
                Win_CommConfig.dcb.BaudRate=CBR_1200;
                break;

            /*1800 baud*/
            case BAUD1800:
                TTY_WARNING("QextSerialPort: Windows does not support 1800 baud operation.  Switching to 1200 baud.");
                Win_CommConfig.dcb.BaudRate=CBR_1200;
                break;

            /*2400 baud*/
            case BAUD2400:
                Win_CommConfig.dcb.BaudRate=CBR_2400;
                break;

            /*4800 baud*/
            case BAUD4800:
                Win_CommConfig.dcb.BaudRate=CBR_4800;
                break;

            /*9600 baud*/
            case BAUD9600:
                Win_CommConfig.dcb.BaudRate=CBR_9600;
                break;

            /*14400 baud*/
            case BAUD14400:
                TTY_PORTABILITY_WARNING("QextSerialPort Portability Warning: POSIX does not support 14400 baud operation.");
                Win_CommConfig.dcb.BaudRate=CBR_14400;
                break;

            /*19200 baud*/
            case BAUD19200:
                Win_CommConfig.dcb.BaudRate=CBR_19200;
                break;

            /*38400 baud*/
            case BAUD38400:
                Win_CommConfig.dcb.BaudRate=CBR_38400;
                break;

            /*56000 baud*/
            case BAUD56000:
                TTY_PORTABILITY_WARNING("QextSerialPort Portability Warning: POSIX does not support 56000 baud operation.");
                Win_CommConfig.dcb.BaudRate=CBR_56000;
                break;

            /*57600 baud*/
            case BAUD57600:
                Win_CommConfig.dcb.BaudRate=CBR_57600;
                break;

            /*76800 baud*/
            case BAUD76800:
                TTY_WARNING("QextSerialPort: Windows does not support 76800 baud operation.  Switching to 57600 baud.");
                Win_CommConfig.dcb.BaudRate=CBR_57600;
                break;

            /*115200 baud*/
            case BAUD115200:
                Win_CommConfig.dcb.BaudRate=CBR_115200;
                break;

            /*128000 baud*/
            case BAUD128000:
                TTY_PORTABILITY_WARNING("QextSerialPort Portability Warning: POSIX does not support 128000 baud operation.");
                Win_CommConfig.dcb.BaudRate=CBR_128000;
                break;

            /*256000 baud*/
            case BAUD256000:
                TTY_PORTABILITY_WARNING("QextSerialPort Portability Warning: POSIX does not support 256000 baud operation.");
                Win_CommConfig.dcb.BaudRate=CBR_256000;
                break;
        }
        SetCommConfig(Win_Handle, &Win_CommConfig, sizeof(COMMCONFIG));
    }
}

void QextSerialPortPrivate::setDtr_sys(bool set) {
    QMutexLocker lock(mutex);
    EscapeCommFunction(Win_Handle, set ? SETDTR : CLRDTR);
}

void QextSerialPortPrivate::setRts_sys(bool set) {
    QMutexLocker lock(mutex);
    EscapeCommFunction(Win_Handle, set ? SETRTS : CLRRTS);
}

ulong QextSerialPortPrivate::lineStatus_sys(void) {
    unsigned long Status=0, Temp=0;
    QMutexLocker lock(mutex);
    GetCommModemStatus(Win_Handle, &Temp);
    if (Temp&MS_CTS_ON) {
        Status|=LS_CTS;
    }
    if (Temp&MS_DSR_ON) {
        Status|=LS_DSR;
    }
    if (Temp&MS_RING_ON) {
        Status|=LS_RI;
    }
    if (Temp&MS_RLSD_ON) {
        Status|=LS_DCD;
    }
    return Status;
}

/*
  Triggered when there's activity on our HANDLE.
*/
void QextSerialPort::onWinEvent(HANDLE h)
{
    QMutexLocker lock(mutex);
    if(h == overlap.hEvent) {
        if (eventMask & EV_RXCHAR) {
            if (sender() != this && bytesAvailable() > 0)
                emit readyRead();
        }
        if (eventMask & EV_TXEMPTY) {
            /*
              A write completed.  Run through the list of OVERLAPPED writes, and if
              they completed successfully, take them off the list and delete them.
              Otherwise, leave them on there so they can finish.
            */
            qint64 totalBytesWritten = 0;
            QList<OVERLAPPED*> overlapsToDelete;
            foreach(OVERLAPPED* o, pendingWrites) {
                DWORD numBytes = 0;
                if (GetOverlappedResult(Win_Handle, o, & numBytes, false)) {
                    overlapsToDelete.append(o);
                    totalBytesWritten += numBytes;
                } else if( GetLastError() != ERROR_IO_INCOMPLETE ) {
                    overlapsToDelete.append(o);
                    qWarning() << "CommEvent overlapped write error:" << GetLastError();
                }
            }

            if (sender() != this && totalBytesWritten > 0) {
                QWriteLocker writelocker(bytesToWriteLock);
                emit bytesWritten(totalBytesWritten);
                _bytesToWrite = 0;
            }

            foreach(OVERLAPPED* o, overlapsToDelete) {
                OVERLAPPED *toDelete = pendingWrites.takeAt(pendingWrites.indexOf(o));
                CloseHandle(toDelete->hEvent);
                delete toDelete;
            }
        }
        if (eventMask & EV_DSR) {
            if (lineStatus() & LS_DSR)
                emit dsrChanged(true);
            else
                emit dsrChanged(false);
        }
    }
    WaitCommEvent(Win_Handle, &eventMask, &overlap);
}

void QextSerialPortPrivate::setTimeout(long millisec) {
    QMutexLocker lock(mutex);
    Settings.Timeout_Millisec = millisec;

    if (millisec == -1) {
        Win_CommTimeouts.ReadIntervalTimeout = MAXDWORD;
        Win_CommTimeouts.ReadTotalTimeoutConstant = 0;
    } else {
        Win_CommTimeouts.ReadIntervalTimeout = millisec;
        Win_CommTimeouts.ReadTotalTimeoutConstant = millisec;
    }
    Win_CommTimeouts.ReadTotalTimeoutMultiplier = 0;
    Win_CommTimeouts.WriteTotalTimeoutMultiplier = millisec;
    Win_CommTimeouts.WriteTotalTimeoutConstant = 0;
    if (queryMode() != QextSerialPort::EventDriven)
        SetCommTimeouts(Win_Handle, &Win_CommTimeouts);
}
