/*
  SPDX-FileCopyrightText: 2007 Klarälvdalens Datakonsult AB

  SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <config-kleopatra.h>

#include "kdpipeiodevice.h"

#include <QDebug>
#include <QMutex>
#include <QPointer>
#include <QThread>
#include <QWaitCondition>
#include "kleopatra_debug.h"

#include <cstring>
#include <memory>
#include <algorithm>

#ifdef Q_OS_WIN32
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
# include <io.h>
#else
# include <unistd.h>
# include <errno.h>
#endif

#ifndef KDAB_CHECK_THIS
# define KDAB_CHECK_CTOR (void)1
# define KDAB_CHECK_DTOR KDAB_CHECK_CTOR
# define KDAB_CHECK_THIS KDAB_CHECK_CTOR
#endif

#define LOCKED( d ) const QMutexLocker locker( &d->mutex )
#define synchronized( d ) if ( int i = 0 ) {} else for ( const QMutexLocker locker( &d->mutex ) ; !i ; ++i )

const unsigned int BUFFER_SIZE = 4096;
const bool ALLOW_QIODEVICE_BUFFERING = true;

namespace
{
KDPipeIODevice::DebugLevel s_debugLevel = KDPipeIODevice::NoDebug;
}

#define QDebug if( s_debugLevel == KDPipeIODevice::NoDebug ){}else qDebug

namespace
{

class Reader : public QThread
{
    Q_OBJECT
public:
    Reader(int fd, Qt::HANDLE handle);
    ~Reader() override;

    qint64 readData(char *data, qint64 maxSize);

    unsigned int bytesInBuffer() const
    {
        return (wptr + sizeof buffer - rptr) % sizeof buffer;
    }

    bool bufferFull() const
    {
        return bytesInBuffer() == sizeof buffer - 1;
    }

    bool bufferEmpty() const
    {
        return bytesInBuffer() == 0;
    }

    bool bufferContains(char ch)
    {
        const unsigned int bib = bytesInBuffer();
        for (unsigned int i = rptr; i < rptr + bib; ++i)
            if (buffer[i % sizeof buffer] == ch) {
                return true;
            }
        return false;
    }

    void notifyReadyRead();

Q_SIGNALS:
    void readyRead();

protected:
    void run() override;

private:
    int fd;
    Qt::HANDLE handle;
public:
    QMutex mutex;
    QWaitCondition waitForCancelCondition;
    QWaitCondition bufferNotFullCondition;
    QWaitCondition bufferNotEmptyCondition;
    QWaitCondition hasStarted;
    QWaitCondition readyReadSentCondition;
    QWaitCondition blockedConsumerIsDoneCondition;
    bool cancel;
    bool eof;
    bool error;
    bool eofShortCut;
    int errorCode;
    bool isReading;
    bool consumerBlocksOnUs;

private:
    unsigned int rptr, wptr;
    char buffer[BUFFER_SIZE + 1]; // need to keep one byte free to detect empty state
};

Reader::Reader(int fd_, Qt::HANDLE handle_) : QThread(),
    fd(fd_),
    handle(handle_),
    mutex(),
    bufferNotFullCondition(),
    bufferNotEmptyCondition(),
    hasStarted(),
    cancel(false),
    eof(false),
    error(false),
    eofShortCut(false),
    errorCode(0),
    isReading(false),
    consumerBlocksOnUs(false),
    rptr(0),
    wptr(0)
{

}

Reader::~Reader() {}

class Writer : public QThread
{
    Q_OBJECT
public:
    Writer(int fd, Qt::HANDLE handle);
    ~Writer() override;

    qint64 writeData(const char *data, qint64 size);

    unsigned int bytesInBuffer() const
    {
        return numBytesInBuffer;
    }

    bool bufferFull() const
    {
        return numBytesInBuffer == sizeof buffer;
    }

    bool bufferEmpty() const
    {
        return numBytesInBuffer == 0;
    }

Q_SIGNALS:
    void bytesWritten(qint64);

protected:
    void run() override;

private:
    int fd;
    Qt::HANDLE handle;
public:
    QMutex mutex;
    QWaitCondition bufferEmptyCondition;
    QWaitCondition bufferNotEmptyCondition;
    QWaitCondition hasStarted;
    bool cancel;
    bool error;
    int errorCode;
private:
    unsigned int numBytesInBuffer;
    char buffer[BUFFER_SIZE];
};
}

Writer::Writer(int fd_, Qt::HANDLE handle_) : QThread(),
    fd(fd_),
    handle(handle_),
    mutex(),
    bufferEmptyCondition(),
    bufferNotEmptyCondition(),
    hasStarted(),
    cancel(false),
    error(false),
    errorCode(0),
    numBytesInBuffer(0)
{

}

Writer::~Writer() {}

class KDPipeIODevice::Private : public QObject
{
    Q_OBJECT
    friend class ::KDPipeIODevice;
    KDPipeIODevice *const q;
public:

    explicit Private(KDPipeIODevice *qq);
    ~Private() override;

    bool doOpen(int, Qt::HANDLE, OpenMode);
    bool startReaderThread();
    bool startWriterThread();
    void stopThreads();

public Q_SLOTS:
    void emitReadyRead();

private:
    int fd;
    Qt::HANDLE handle;
    Reader *reader;
    Writer *writer;
    bool triedToStartReader;
    bool triedToStartWriter;
};

KDPipeIODevice::DebugLevel KDPipeIODevice::debugLevel()
{
    return s_debugLevel;
}

void KDPipeIODevice::setDebugLevel(KDPipeIODevice::DebugLevel level)
{
    s_debugLevel = level;
}

KDPipeIODevice::Private::Private(KDPipeIODevice *qq) : QObject(qq), q(qq),
    fd(-1),
    handle(nullptr),
    reader(nullptr),
    writer(nullptr),
    triedToStartReader(false),
    triedToStartWriter(false)
{

}

KDPipeIODevice::Private::~Private()
{
    QDebug("KDPipeIODevice::~Private(): Destroying %p", (void *) q);
}

KDPipeIODevice::KDPipeIODevice(QObject *p)
    : QIODevice(p), d(new Private(this))
{
    KDAB_CHECK_CTOR;
}

KDPipeIODevice::KDPipeIODevice(int fd, OpenMode mode, QObject *p)
    : QIODevice(p), d(new Private(this))
{
    KDAB_CHECK_CTOR;
    open(fd, mode);
}

KDPipeIODevice::KDPipeIODevice(Qt::HANDLE handle, OpenMode mode, QObject *p)
    : QIODevice(p), d(new Private(this))
{
    KDAB_CHECK_CTOR;
    open(handle, mode);
}

KDPipeIODevice::~KDPipeIODevice()
{
    KDAB_CHECK_DTOR;
    if (isOpen()) {
        close();
    }
    delete d; d = nullptr;
}

bool KDPipeIODevice::open(int fd, OpenMode mode)
{
    KDAB_CHECK_THIS;
#ifdef Q_OS_WIN32
    return d->doOpen(fd, (HANDLE)_get_osfhandle(fd), mode);
#else
    return d->doOpen(fd, nullptr, mode);
#endif
}

bool KDPipeIODevice::open(Qt::HANDLE h, OpenMode mode)
{
    KDAB_CHECK_THIS;
#ifdef Q_OS_WIN32
    return d->doOpen(-1, h, mode);
#else
    Q_UNUSED(h)
    Q_UNUSED(mode)
    Q_ASSERT(!"KDPipeIODevice::open( Qt::HANDLE, OpenMode ) should never be called except on Windows.");
    return false;
#endif
}

bool KDPipeIODevice::Private::startReaderThread()
{
    if (triedToStartReader) {
        return true;
    }
    triedToStartReader = true;
    if (reader && !reader->isRunning() && !reader->isFinished()) {
        QDebug("KDPipeIODevice::Private::startReaderThread(): locking reader (CONSUMER THREAD)");
        LOCKED(reader);
        QDebug("KDPipeIODevice::Private::startReaderThread(): locked reader (CONSUMER THREAD)");
        reader->start(QThread::HighestPriority);
        QDebug("KDPipeIODevice::Private::startReaderThread(): waiting for hasStarted (CONSUMER THREAD)");
        const bool hasStarted = reader->hasStarted.wait(&reader->mutex, 1000);
        QDebug("KDPipeIODevice::Private::startReaderThread(): returned from hasStarted (CONSUMER THREAD)");

        return hasStarted;
    }
    return true;
}

bool KDPipeIODevice::Private::startWriterThread()
{
    if (triedToStartWriter) {
        return true;
    }
    triedToStartWriter = true;
    if (writer && !writer->isRunning() && !writer->isFinished()) {
        LOCKED(writer);

        writer->start(QThread::HighestPriority);
        if (!writer->hasStarted.wait(&writer->mutex, 1000)) {
            return false;
        }
    }
    return true;
}

void KDPipeIODevice::Private::emitReadyRead()
{
    QPointer<Private> thisPointer(this);
    QDebug("KDPipeIODevice::Private::emitReadyRead %p", (void *) this);

    Q_EMIT q->readyRead();

    if (!thisPointer) {
        return;
    }
    if (reader) {
        QDebug("KDPipeIODevice::Private::emitReadyRead %p: locking reader (CONSUMER THREAD)", (
                   void *) this);
        synchronized(reader) {
            QDebug("KDPipeIODevice::Private::emitReadyRead %p: locked reader (CONSUMER THREAD)", (
                       void *) this);
            reader->readyReadSentCondition.wakeAll();
            QDebug("KDPipeIODevice::Private::emitReadyRead %p: buffer empty: %d reader in ReadFile: %d", (void *)this, reader->bufferEmpty(), reader->isReading);
        }
    }
    QDebug("KDPipeIODevice::Private::emitReadyRead %p leaving", (void *) this);

}

bool KDPipeIODevice::Private::doOpen(int fd_, Qt::HANDLE handle_, OpenMode mode_)
{

    if (q->isOpen()) {
        return false;
    }

#ifdef Q_OS_WIN32
    if (!handle_) {
        return false;
    }
#else
    if (fd_ < 0) {
        return false;
    }
#endif

    if (!(mode_ & ReadWrite)) {
        return false;    // need to have at least read -or- write
    }

    std::unique_ptr<Reader> reader_;
    std::unique_ptr<Writer> writer_;

    if (mode_ & ReadOnly) {
        reader_ = std::make_unique<Reader>(fd_, handle_);
        QDebug("KDPipeIODevice::doOpen (%p): created reader (%p) for fd %d", (void *)this,
               (void *)reader_.get(), fd_);
        connect(reader_.get(), &Reader::readyRead, this, &Private::emitReadyRead,
                Qt::QueuedConnection);
    }
    if (mode_ & WriteOnly) {
        writer_ = std::make_unique<Writer>(fd_, handle_);
        QDebug("KDPipeIODevice::doOpen (%p): created writer (%p) for fd %d",
               (void *)this, (void *)writer_.get(), fd_);
        connect(writer_.get(), &Writer::bytesWritten, q, &QIODevice::bytesWritten,
                Qt::QueuedConnection);
    }

    // commit to *this:
    fd = fd_;
    handle = handle_;
    reader = reader_.release();
    writer = writer_.release();

    q->setOpenMode(mode_ | Unbuffered);
    return true;
}

int KDPipeIODevice::descriptor() const
{
    KDAB_CHECK_THIS;
    return d->fd;
}

Qt::HANDLE KDPipeIODevice::handle() const
{
    KDAB_CHECK_THIS;
    return d->handle;
}

qint64 KDPipeIODevice::bytesAvailable() const
{
    KDAB_CHECK_THIS;
    const qint64 base = QIODevice::bytesAvailable();
    if (!d->triedToStartReader) {
        d->startReaderThread();
        return base;
    }
    if (d->reader) {
        synchronized(d->reader) {
            const qint64 inBuffer = d->reader->bytesInBuffer();
            return base + inBuffer;
        }
    }
    return base;
}

qint64 KDPipeIODevice::bytesToWrite() const
{
    KDAB_CHECK_THIS;
    d->startWriterThread();
    const qint64 base = QIODevice::bytesToWrite();
    if (d->writer) {
        synchronized(d->writer) return base + d->writer->bytesInBuffer();
    }
    return base;
}

bool KDPipeIODevice::canReadLine() const
{
    KDAB_CHECK_THIS;
    d->startReaderThread();
    if (QIODevice::canReadLine()) {
        return true;
    }
    if (d->reader) {
        synchronized(d->reader) return d->reader->bufferContains('\n');
    }
    return true;
}

bool KDPipeIODevice::isSequential() const
{
    return true;
}

bool KDPipeIODevice::atEnd() const
{
    KDAB_CHECK_THIS;
    d->startReaderThread();
    if (!QIODevice::atEnd()) {
        QDebug("%p: KDPipeIODevice::atEnd returns false since QIODevice::atEnd does (with bytesAvailable=%ld)", (void *)this, static_cast<long>(bytesAvailable()));
        return false;
    }
    if (!isOpen()) {
        return true;
    }
    if (d->reader->eofShortCut) {
        return true;
    }
    LOCKED(d->reader);
    const bool eof = (d->reader->error || d->reader->eof) && d->reader->bufferEmpty();
    if (!eof) {
        if (!d->reader->error && !d->reader->eof) {
            QDebug("%p: KDPipeIODevice::atEnd returns false since !reader->error && !reader->eof",
                   (void *)(this));
        }
        if (!d->reader->bufferEmpty()) {
            QDebug("%p: KDPipeIODevice::atEnd returns false since !reader->bufferEmpty()",
                   (void *)  this);
        }
    }
    return eof;
}

bool KDPipeIODevice::waitForBytesWritten(int msecs)
{
    KDAB_CHECK_THIS;
    d->startWriterThread();
    Writer *const w = d->writer;
    if (!w) {
        return true;
    }
    LOCKED(w);
    QDebug("KDPipeIODevice::waitForBytesWritten (%p,w=%p): entered locked area",
           (void *)this, (void *) w);
    return w->bufferEmpty() || w->error || w->bufferEmptyCondition.wait(&w->mutex, msecs);
}

bool KDPipeIODevice::waitForReadyRead(int msecs)
{
    KDAB_CHECK_THIS;
    QDebug("KDPipeIODEvice::waitForReadyRead()(%p)", (void *) this);
    d->startReaderThread();
    if (ALLOW_QIODEVICE_BUFFERING) {
        if (bytesAvailable() > 0) {
            return true;
        }
    }
    Reader *const r = d->reader;
    if (!r || r->eofShortCut) {
        return true;
    }
    LOCKED(r);
    if (r->bytesInBuffer() != 0 || r->eof || r->error) {
        return true;
    }
    Q_ASSERT(false);   // ### wtf?
    return r->bufferNotEmptyCondition.wait(&r->mutex, msecs);
}

template <typename T>
class TemporaryValue
{
public:
    TemporaryValue(T &var_, const T &tv) : var(var_), oldValue(var_)
    {
        var = tv;
    }
    ~TemporaryValue()
    {
        var = oldValue;
    }
private:
    T &var;
    const T oldValue;
};

bool KDPipeIODevice::readWouldBlock() const
{
    d->startReaderThread();
    LOCKED(d->reader);
    return d->reader->bufferEmpty() && !d->reader->eof && !d->reader->error;
}

bool KDPipeIODevice::writeWouldBlock() const
{
    d->startWriterThread();
    LOCKED(d->writer);
    return !d->writer->bufferEmpty() && !d->writer->error;
}

qint64 KDPipeIODevice::readData(char *data, qint64 maxSize)
{
    KDAB_CHECK_THIS;
    QDebug("%p: KDPipeIODevice::readData: data=%s, maxSize=%lld", (void *)this, data, maxSize);
    d->startReaderThread();
    Reader *const r = d->reader;

    Q_ASSERT(r);

    //assert( r->isRunning() ); // wrong (might be eof, error)
    Q_ASSERT(data || maxSize == 0);
    Q_ASSERT(maxSize >= 0);

    if (r->eofShortCut) {
        QDebug("%p: KDPipeIODevice::readData: hit eofShortCut, returning 0", (void *)this);
        return 0;
    }

    if (maxSize < 0) {
        maxSize = 0;
    }

    if (ALLOW_QIODEVICE_BUFFERING) {
        if (bytesAvailable() > 0) {
            maxSize = std::min(maxSize, bytesAvailable());    // don't block
        }
    }
    QDebug("%p: KDPipeIODevice::readData: try to lock reader (CONSUMER THREAD)", (void *) this);
    LOCKED(r);
    QDebug("%p: KDPipeIODevice::readData: locked reader (CONSUMER THREAD)", (void *) this);

    r->readyReadSentCondition.wakeAll();
    if (/* maxSize > 0 && */ r->bufferEmpty() &&  !r->error && !r->eof) {   // ### block on maxSize == 0?
        QDebug("%p: KDPipeIODevice::readData: waiting for bufferNotEmptyCondition (CONSUMER THREAD)", (void *) this);
        const TemporaryValue<bool> tmp(d->reader->consumerBlocksOnUs, true);
        r->bufferNotEmptyCondition.wait(&r->mutex);
        r->blockedConsumerIsDoneCondition.wakeAll();
        QDebug("%p: KDPipeIODevice::readData: woke up from bufferNotEmptyCondition (CONSUMER THREAD)",
               (void *) this);
    }

    if (r->bufferEmpty()) {
        QDebug("%p: KDPipeIODevice::readData: got empty buffer, signal eof", (void *) this);
        // woken with an empty buffer must mean either EOF or error:
        Q_ASSERT(r->eof || r->error);
        r->eofShortCut = true;
        return r->eof ? 0 : -1;
    }

    QDebug("%p: KDPipeIODevice::readData: got bufferNotEmptyCondition, trying to read %lld bytes",
           (void *)this, maxSize);
    const qint64 bytesRead = r->readData(data, maxSize);
    QDebug("%p: KDPipeIODevice::readData: read %lld bytes", (void *)this, bytesRead);
    QDebug("%p (fd=%d): KDPipeIODevice::readData: %s", (void *)this, d->fd, data);

    return bytesRead;
}

qint64 Reader::readData(char *data, qint64 maxSize)
{
    qint64 numRead = rptr < wptr ? wptr - rptr : sizeof buffer - rptr;
    if (numRead > maxSize) {
        numRead = maxSize;
    }

    QDebug("%p: KDPipeIODevice::readData: data=%s, maxSize=%lld; rptr=%u, wptr=%u (bytesInBuffer=%u); -> numRead=%lld",
           (void *)this, data, maxSize, rptr, wptr, bytesInBuffer(), numRead);

    memcpy(data, buffer + rptr, numRead);

    rptr = (rptr + numRead) % sizeof buffer;

    if (!bufferFull()) {
        QDebug("%p: KDPipeIODevice::readData: signal bufferNotFullCondition", (void *) this);
        bufferNotFullCondition.wakeAll();
    }

    return numRead;
}

qint64 KDPipeIODevice::writeData(const char *data, qint64 size)
{
    KDAB_CHECK_THIS;
    d->startWriterThread();
    Writer *const w = d->writer;

    Q_ASSERT(w);
    Q_ASSERT(w->error || w->isRunning());
    Q_ASSERT(data || size == 0);
    Q_ASSERT(size >= 0);

    LOCKED(w);

    while (!w->error && !w->bufferEmpty()) {
        QDebug("%p: KDPipeIODevice::writeData: wait for empty buffer", (void *) this);
        w->bufferEmptyCondition.wait(&w->mutex);
        QDebug("%p: KDPipeIODevice::writeData: empty buffer signaled", (void *) this);

    }
    if (w->error) {
        return -1;
    }

    Q_ASSERT(w->bufferEmpty());

    return w->writeData(data, size);
}

qint64 Writer::writeData(const char *data, qint64 size)
{
    Q_ASSERT(bufferEmpty());

    if (size > static_cast<qint64>(sizeof buffer)) {
        size = sizeof buffer;
    }

    memcpy(buffer, data, size);

    numBytesInBuffer = size;

    if (!bufferEmpty()) {
        bufferNotEmptyCondition.wakeAll();
    }
    return size;
}

void KDPipeIODevice::Private::stopThreads()
{
    if (triedToStartWriter) {
        if (writer && q->bytesToWrite() > 0) {
            q->waitForBytesWritten(-1);
        }

        Q_ASSERT(q->bytesToWrite() == 0);
    }
    if (Reader *&r = reader) {
        disconnect(r, &Reader::readyRead, this, &Private::emitReadyRead);
        synchronized(r) {
            // tell thread to cancel:
            r->cancel = true;
            // and wake it, so it can terminate:
            r->waitForCancelCondition.wakeAll();
            r->bufferNotFullCondition.wakeAll();
            r->readyReadSentCondition.wakeAll();
        }
    }
    if (Writer *&w = writer) {
        synchronized(w) {
            // tell thread to cancel:
            w->cancel = true;
            // and wake it, so it can terminate:
            w->bufferNotEmptyCondition.wakeAll();
        }
    }
}

void KDPipeIODevice::close()
{
    KDAB_CHECK_THIS;
    QDebug("KDPipeIODevice::close(%p)", (void *) this);
    if (!isOpen()) {
        return;
    }

    // tell clients we're about to close:
    Q_EMIT aboutToClose();
    d->stopThreads();

#define waitAndDelete( t ) if ( t ) { t->wait(); QThread* const t2 = t; t = nullptr; delete t2; }
    QDebug("KPipeIODevice::close(%p): wait and closing writer %p", (void *)this, (void *) d->writer);
    waitAndDelete(d->writer);
    QDebug("KPipeIODevice::close(%p): wait and closing reader %p", (void *)this, (void *) d->reader);
    if (d->reader) {
        LOCKED(d->reader);
        d->reader->readyReadSentCondition.wakeAll();
    }
    waitAndDelete(d->reader);
#undef waitAndDelete
#ifdef Q_OS_WIN32
    if (d->fd != -1) {
        _close(d->fd);
    } else {
        CloseHandle(d->handle);
    }
#else
    ::close(d->fd);
#endif

    setOpenMode(NotOpen);
    d->fd = -1;
    d->handle = nullptr;
}

void Reader::run()
{

    LOCKED(this);

    // too bad QThread doesn't have that itself; a signal isn't enough
    hasStarted.wakeAll();

    QDebug("%p: Reader::run: started", (void *) this);

    while (true) {
        if (!cancel && (eof || error)) {
            //notify the client until the buffer is empty and then once
            //again so he receives eof/error. After that, wait for him
            //to cancel
            const bool wasEmpty = bufferEmpty();
            QDebug("%p: Reader::run: received eof(%d) or error(%d), waking everyone", (void *)this, eof, error);
            notifyReadyRead();
            if (!cancel && wasEmpty) {
                waitForCancelCondition.wait(&mutex);
            }
        } else if (!cancel && !bufferFull() && !bufferEmpty()) {
            QDebug("%p: Reader::run: buffer no longer empty, waking everyone", (void *) this);
            notifyReadyRead();
        }

        while (!cancel && !error && bufferFull()) {
            notifyReadyRead();
            if (!cancel && bufferFull()) {
                QDebug("%p: Reader::run: buffer is full, going to sleep", (void *)this);
                bufferNotFullCondition.wait(&mutex);
            }
        }

        if (cancel) {
            QDebug("%p: Reader::run: detected cancel", (void *)this);
            goto leave;
        }

        if (!eof && !error) {
            if (rptr == wptr) { // optimize for larger chunks in case the buffer is empty
                rptr = wptr = 0;
            }

            unsigned int numBytes = (rptr + sizeof buffer - wptr - 1) % sizeof buffer;
            if (numBytes > sizeof buffer - wptr) {
                numBytes = sizeof buffer - wptr;
            }

            QDebug("%p: Reader::run: rptr=%d, wptr=%d -> numBytes=%d", (void *)this, rptr, wptr, numBytes);

            Q_ASSERT(numBytes > 0);

            QDebug("%p: Reader::run: trying to read %d bytes from fd %d", (void *)this, numBytes, fd);
#ifdef Q_OS_WIN32
            isReading = true;
            mutex.unlock();
            DWORD numRead;
            const bool ok = ReadFile(handle, buffer + wptr, numBytes, &numRead, 0);
            mutex.lock();
            isReading = false;
            if (ok) {
                if (numRead == 0) {
                    QDebug("%p: Reader::run: got eof (numRead==0)", (void *) this);
                    eof = true;
                }
            } else { // !ok
                errorCode = static_cast<int>(GetLastError());
                if (errorCode == ERROR_BROKEN_PIPE) {
                    Q_ASSERT(numRead == 0);
                    QDebug("%p: Reader::run: got eof (broken pipe)", (void *) this);
                    eof = true;
                } else {
                    Q_ASSERT(numRead == 0);
                    QDebug("%p: Reader::run: got error: %s (%d)", (void *) this, strerror(errorCode), errorCode);
                    error = true;
                }
            }
#else
            qint64 numRead;
            mutex.unlock();
            do {
                numRead = ::read(fd, buffer + wptr, numBytes);
            } while (numRead == -1 && errno == EINTR);
            mutex.lock();

            if (numRead < 0) {
                errorCode = errno;
                error = true;
                QDebug("%p: Reader::run: got error: %d", (void *)this, errorCode);
            } else if (numRead == 0) {
                QDebug("%p: Reader::run: eof detected", (void *)this);
                eof = true;
            }
#endif
            QDebug("%p (fd=%d): Reader::run: read %ld bytes", (void *) this, fd, static_cast<long>(numRead));
            QDebug("%p (fd=%d): Reader::run: %s", (void *)this, fd, buffer);

            if (numRead > 0) {
                QDebug("%p: Reader::run: buffer before: rptr=%4d, wptr=%4d", (void *)this, rptr, wptr);
                wptr = (wptr + numRead) % sizeof buffer;
                QDebug("%p: Reader::run: buffer after:  rptr=%4d, wptr=%4d", (void *)this, rptr, wptr);
            }
        }
    }
leave:
    QDebug("%p: Reader::run: terminated", (void *)this);
}

void Reader::notifyReadyRead()
{
    QDebug("notifyReadyRead: %d bytes available", bytesInBuffer());
    Q_ASSERT(!cancel);

    if (consumerBlocksOnUs) {
        bufferNotEmptyCondition.wakeAll();
        blockedConsumerIsDoneCondition.wait(&mutex);
        return;
    }
    QDebug("notifyReadyRead: Q_EMIT signal");
    Q_EMIT readyRead();
    readyReadSentCondition.wait(&mutex);
    QDebug("notifyReadyRead: returning from waiting, leave");
}

void Writer::run()
{

    LOCKED(this);

    // too bad QThread doesn't have that itself; a signal isn't enough
    hasStarted.wakeAll();

    qCDebug(KLEOPATRA_LOG) << this << "Writer::run: started";

    while (true) {

        while (!cancel && bufferEmpty()) {
            qCDebug(KLEOPATRA_LOG) << this << "Writer::run: buffer is empty, wake bufferEmptyCond listeners";
            bufferEmptyCondition.wakeAll();
            Q_EMIT bytesWritten(0);
            qCDebug(KLEOPATRA_LOG) << this << "Writer::run: buffer is empty, going to sleep";
            bufferNotEmptyCondition.wait(&mutex);
            qCDebug(KLEOPATRA_LOG) << this << "Writer::run: woke up";
        }

        if (cancel) {
            qCDebug(KLEOPATRA_LOG) << this <<  "Writer::run: detected cancel";
            goto leave;
        }

        Q_ASSERT(numBytesInBuffer > 0);

        qCDebug(KLEOPATRA_LOG) << this << "Writer::run: Trying to write " << numBytesInBuffer << "bytes";
        qint64 totalWritten = 0;
        do {
            mutex.unlock();
#ifdef Q_OS_WIN32
            DWORD numWritten;
            QDebug("%p (fd=%d): Writer::run: buffer before WriteFile (numBytes=%lld): %s:",
                   (void *) this, fd, numBytesInBuffer, buffer);
            QDebug("%p (fd=%d): Writer::run: Going into WriteFile", (void *) this, fd);
            if (!WriteFile(handle, buffer + totalWritten, numBytesInBuffer - totalWritten, &numWritten, 0)) {
                mutex.lock();
                errorCode = static_cast<int>(GetLastError());
                QDebug("%p: Writer::run: got error code: %d", (void *) this, errorCode);
                error = true;
                goto leave;
            }
#else
            qint64 numWritten;
            do {
                numWritten = ::write(fd, buffer + totalWritten, numBytesInBuffer - totalWritten);
            } while (numWritten == -1 && errno == EINTR);

            if (numWritten < 0) {
                mutex.lock();
                errorCode = errno;
                QDebug("%p: Writer::run: got error code: %s (%d)", (void *)this, strerror(errorCode), errorCode);
                error = true;
                goto leave;
            }
#endif
            QDebug("%p (fd=%d): Writer::run: buffer after WriteFile (numBytes=%u): %s:", (void *)this, fd, numBytesInBuffer, buffer);
            totalWritten += numWritten;
            mutex.lock();
        } while (totalWritten < numBytesInBuffer);

        qCDebug(KLEOPATRA_LOG) << this << "Writer::run: wrote " << totalWritten << "bytes";
        numBytesInBuffer = 0;
        qCDebug(KLEOPATRA_LOG) << this << "Writer::run: buffer is empty, wake bufferEmptyCond listeners";
        bufferEmptyCondition.wakeAll();
        Q_EMIT bytesWritten(totalWritten);
    }
leave:
    qCDebug(KLEOPATRA_LOG) << this << "Writer::run: terminating";
    numBytesInBuffer = 0;
    qCDebug(KLEOPATRA_LOG) << this << "Writer::run: buffer is empty, wake bufferEmptyCond listeners";
    bufferEmptyCondition.wakeAll();
    Q_EMIT bytesWritten(0);
}

// static
std::pair<KDPipeIODevice *, KDPipeIODevice *> KDPipeIODevice::makePairOfConnectedPipes()
{
    KDPipeIODevice *read = nullptr;
    KDPipeIODevice *write = nullptr;
#ifdef Q_OS_WIN32
    HANDLE rh;
    HANDLE wh;
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (CreatePipe(&rh, &wh, &sa, BUFFER_SIZE)) {
        read = new KDPipeIODevice;
        read->open(rh, ReadOnly);
        write = new KDPipeIODevice;
        write->open(wh, WriteOnly);
    }
#else
    int fds[2];
    if (pipe(fds) == 0) {
        read = new KDPipeIODevice;
        read->open(fds[0], ReadOnly);
        write = new KDPipeIODevice;
        write->open(fds[1], WriteOnly);
    }
#endif
    return std::make_pair(read, write);
}

#ifdef KDAB_DEFINE_CHECKS
KDAB_DEFINE_CHECKS(KDPipeIODevice)
{
    if (!isOpen()) {
        Q_ASSERT(openMode() == NotOpen);
        Q_ASSERT(!d->reader);
        Q_ASSERT(!d->writer);
#ifdef Q_OS_WIN32
        Q_ASSERT(!d->handle);
#else
        Q_ASSERT(d->fd < 0);
#endif
    } else {
        Q_ASSERT(openMode() != NotOpen);
        Q_ASSERT(openMode() & ReadWrite);
        if (openMode() & ReadOnly) {
            Q_ASSERT(d->reader);
            synchronized(d->reader)
            Q_ASSERT(d->reader->eof || d->reader->error || d->reader->isRunning());
        }
        if (openMode() & WriteOnly) {
            Q_ASSERT(d->writer);
            synchronized(d->writer)
            Q_ASSERT(d->writer->error || d->writer->isRunning());
        }
#ifdef Q_OS_WIN32
        Q_ASSERT(d->handle);
#else
        Q_ASSERT(d->fd >= 0);
#endif
    }
}
#endif // KDAB_DEFINE_CHECKS

#include "kdpipeiodevice.moc"
