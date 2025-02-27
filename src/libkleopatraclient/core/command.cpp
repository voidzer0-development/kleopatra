/* -*- mode: c++; c-basic-offset:4 -*-
    command.cpp

    This file is part of KleopatraClient, the Kleopatra interface library
    SPDX-FileCopyrightText: 2008 Klarälvdalens Datakonsult AB

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include <config-kleopatra.h>

#include "command.h"
#include "command_p.h"

#include <QtGlobal> // Q_OS_WIN

#ifdef Q_OS_WIN // HACK: AllowSetForegroundWindow needs _WIN32_WINDOWS >= 0x0490 set
# ifndef _WIN32_WINDOWS
#  define _WIN32_WINDOWS 0x0500
#  define _WIN32_WINNT 0x0500 // good enough for Vista too
# endif
# include <utils/gnupg-registry.h>
# include <windows.h>
#endif

#include <QMutexLocker>
#include <QFile>
#include "libkleopatraclientcore_debug.h"
#include <QDir>
#include <QProcess>
#include <KLocalizedString>

#include <assuan.h>
#include <gpg-error.h>
#include <gpgme++/global.h>

#include <algorithm>
#include <string>
#include <sstream>
#include <memory>
#include <type_traits>

using namespace KleopatraClientCopy;

// copied from kleopatra/utils/hex.cpp
static std::string hexencode(const std::string &in)
{
    std::string result;
    result.reserve(3 * in.size());

    static const char hex[] = "0123456789ABCDEF";

    for (std::string::const_iterator it = in.begin(), end = in.end(); it != end; ++it)
        switch (const unsigned char ch = *it) {
        default:
            if ((ch >= '!' && ch <= '~') || ch > 0xA0) {
                result += ch;
                break;
            }
            Q_FALLTHROUGH();
        // else fall through
        case ' ':
            result += '+';
            break;
        case '"':
        case '#':
        case '$':
        case '%':
        case '\'':
        case '+':
        case '=':
            result += '%';
            result += hex[(ch & 0xF0) >> 4 ];
            result += hex[(ch & 0x0F)      ];
            break;
        }

    return result;
}

#ifdef UNUSED
static std::string hexencode(const char *in)
{
    if (!in) {
        return std::string();
    }
    return hexencode(std::string(in));
}
#endif

// changed from returning QByteArray to returning std::string
static std::string hexencode(const QByteArray &in)
{
    if (in.isNull()) {
        return std::string();
    }
    return hexencode(std::string(in.data(), in.size()));
}
// end copied from kleopatra/utils/hex.cpp

Command::Command(QObject *p)
    : QObject(p), d(new Private(this))
{
    d->init();
}

Command::Command(Private *pp, QObject *p)
    : QObject(p), d(pp)
{
    d->init();
}

Command::~Command()
{
    delete d; d = nullptr;
}

void Command::Private::init()
{
    connect(this, &QThread::started,  q, &Command::started);
    connect(this, &QThread::finished, q, &Command::finished);
}

void Command::setParentWId(WId wid)
{
    const QMutexLocker locker(&d->mutex);
    d->inputs.parentWId = wid;
}

WId Command::parentWId() const
{
    const QMutexLocker locker(&d->mutex);
    return d->inputs.parentWId;
}

void Command::setServerLocation(const QString &location)
{
    const QMutexLocker locker(&d->mutex);
    d->outputs.serverLocation = location;
}

QString Command::serverLocation() const
{
    const QMutexLocker locker(&d->mutex);
    return d->outputs.serverLocation;
}

bool Command::waitForFinished()
{
    return d->wait();
}

bool Command::waitForFinished(unsigned long ms)
{
    return d->wait(ms);
}

bool Command::error() const
{
    const QMutexLocker locker(&d->mutex);
    return !d->outputs.errorString.isEmpty();
}

bool Command::wasCanceled() const
{
    const QMutexLocker locker(&d->mutex);
    return d->outputs.canceled;
}

QString Command::errorString() const
{
    const QMutexLocker locker(&d->mutex);
    return d->outputs.errorString;
}

qint64 Command::serverPid() const
{
    const QMutexLocker locker(&d->mutex);
    return d->outputs.serverPid;
}

void Command::start()
{
    d->start();
}

void Command::cancel()
{
    qCDebug(LIBKLEOPATRACLIENTCORE_LOG) << "Sorry, not implemented: KleopatraClient::Command::Cancel";
}

void Command::setOptionValue(const char *name, const QVariant &value, bool critical)
{
    if (!name || !*name) {
        return;
    }
    const Private::Option opt = {
        value,
        true,
        critical
    };
    const QMutexLocker locker(&d->mutex);
    d->inputs.options[name] = opt;
}

QVariant Command::optionValue(const char *name) const
{
    if (!name || !*name) {
        return QVariant();
    }
    const QMutexLocker locker(&d->mutex);

    const auto it = d->inputs.options.find(name);
    if (it == d->inputs.options.end()) {
        return QVariant();
    } else {
        return it->second.value;
    }
}

void Command::setOption(const char *name, bool critical)
{
    if (!name || !*name) {
        return;
    }
    const QMutexLocker locker(&d->mutex);

    if (isOptionSet(name)) {
        unsetOption(name);
    }

    const Private::Option opt = {
        QVariant(),
        false,
        critical
    };

    d->inputs.options[name] = opt;
}

void Command::unsetOption(const char *name)
{
    if (!name || !*name) {
        return;
    }
    const QMutexLocker locker(&d->mutex);
    d->inputs.options.erase(name);
}

bool Command::isOptionSet(const char *name) const
{
    if (!name || !*name) {
        return false;
    }
    const QMutexLocker locker(&d->mutex);
    return d->inputs.options.count(name);
}

bool Command::isOptionCritical(const char *name) const
{
    if (!name || !*name) {
        return false;
    }
    const QMutexLocker locker(&d->mutex);
    const auto it = d->inputs.options.find(name);
    return it != d->inputs.options.end() && it->second.isCritical;
}

void Command::setFilePaths(const QStringList &filePaths)
{
    const QMutexLocker locker(&d->mutex);
    d->inputs.filePaths = filePaths;
}

QStringList Command::filePaths() const
{
    const QMutexLocker locker(&d->mutex);
    return d->inputs.filePaths;
}

void Command::setRecipients(const QStringList &recipients, bool informative)
{
    const QMutexLocker locker(&d->mutex);
    d->inputs.recipients = recipients;
    d->inputs.areRecipientsInformative = informative;
}

QStringList Command::recipients() const
{
    const QMutexLocker locker(&d->mutex);
    return d->inputs.recipients;
}

bool Command::areRecipientsInformative() const
{
    const QMutexLocker locker(&d->mutex);
    return d->inputs.areRecipientsInformative;
}

void Command::setSenders(const QStringList &senders, bool informative)
{
    const QMutexLocker locker(&d->mutex);
    d->inputs.senders = senders;
    d->inputs.areSendersInformative = informative;
}

QStringList Command::senders() const
{
    const QMutexLocker locker(&d->mutex);
    return d->inputs.senders;
}

bool Command::areSendersInformative() const
{
    const QMutexLocker locker(&d->mutex);
    return d->inputs.areSendersInformative;
}

void Command::setInquireData(const char *what, const QByteArray &data)
{
    const QMutexLocker locker(&d->mutex);
    d->inputs.inquireData[what] = data;
}

void Command::unsetInquireData(const char *what)
{
    const QMutexLocker locker(&d->mutex);
    d->inputs.inquireData.erase(what);
}

QByteArray Command::inquireData(const char *what) const
{
    const QMutexLocker locker(&d->mutex);
    const auto it = d->inputs.inquireData.find(what);
    if (it == d->inputs.inquireData.end()) {
        return QByteArray();
    } else {
        return it->second;
    }
}

bool Command::isInquireDataSet(const char *what) const
{
    const QMutexLocker locker(&d->mutex);
    const auto it = d->inputs.inquireData.find(what);
    return it != d->inputs.inquireData.end();
}

QByteArray Command::receivedData() const
{
    const QMutexLocker locker(&d->mutex);
    return d->outputs.data;
}

void Command::setCommand(const char *command)
{
    const QMutexLocker locker(&d->mutex);
    d->inputs.command = command;
}

QByteArray Command::command() const
{
    const QMutexLocker locker(&d->mutex);
    return d->inputs.command;
}

//
// here comes the ugly part
//

#ifdef HAVE_ASSUAN2
static void my_assuan_release(assuan_context_t ctx)
{
    if (ctx) {
        assuan_release(ctx);
    }
}
#endif

using AssuanContextBase = std::shared_ptr<std::remove_pointer<assuan_context_t>::type>;
namespace
{
struct AssuanClientContext : AssuanContextBase {
    AssuanClientContext() : AssuanContextBase() {}
#ifndef HAVE_ASSUAN2
    explicit AssuanClientContext(assuan_context_t ctx) : AssuanContextBase(ctx, &assuan_disconnect) {}
    void reset(assuan_context_t ctx = nullptr)
    {
        AssuanContextBase::reset(ctx, &assuan_disconnect);
    }
#else
    explicit AssuanClientContext(assuan_context_t ctx) : AssuanContextBase(ctx, &my_assuan_release) {}
    void reset(assuan_context_t ctx = nullptr)
    {
        AssuanContextBase::reset(ctx, &my_assuan_release);
    }
#endif
};
}

#ifdef HAVE_ASSUAN2
// compatibility typedef - remove when we require assuan v2...
using assuan_error_t = gpg_error_t;
#endif

static assuan_error_t
my_assuan_transact(const AssuanClientContext &ctx,
                   const char *command,
                   assuan_error_t (*data_cb)(void *, const void *, size_t) = nullptr,
                   void *data_cb_arg = nullptr,
                   assuan_error_t (*inquire_cb)(void *, const char *) = nullptr,
                   void *inquire_cb_arg = nullptr,
                   assuan_error_t (*status_cb)(void *, const char *) = nullptr,
                   void *status_cb_arg = nullptr)
{
    return assuan_transact(ctx.get(), command, data_cb, data_cb_arg, inquire_cb, inquire_cb_arg, status_cb, status_cb_arg);
}

static QString to_error_string(int err)
{
    char buffer[1024];
    gpg_strerror_r(static_cast<gpg_error_t>(err),
                   buffer, sizeof buffer);
    buffer[sizeof buffer - 1] = '\0';
    return QString::fromLocal8Bit(buffer);
}

static QString gnupg_home_directory()
{
    static const char *hDir = GpgME::dirInfo("homedir");
    return QFile::decodeName(hDir);
}

static QString get_default_socket_name()
{
    const QString socketPath{QString::fromUtf8(GpgME::dirInfo("uiserver-socket"))};
    if (!socketPath.isEmpty()) {
        // Note: The socket directory exists after GpgME::dirInfo() has been called.
        return socketPath;
    }
    const QString homeDir = gnupg_home_directory();
    if (homeDir.isEmpty()) {
        return QString();
    }
    return QDir(homeDir).absoluteFilePath(QStringLiteral("S.uiserver"));
}

static QString default_socket_name()
{
    static QString name = get_default_socket_name();
    return name;
}

static QString uiserver_executable()
{
    return QStringLiteral("kleopatra");
}

static QString start_uiserver()
{
    if (!QProcess::startDetached(uiserver_executable(), QStringList() << QStringLiteral("--daemon"))) {
        return i18n("Failed to start uiserver %1", uiserver_executable());
    } else {
        return QString();
    }
}

static assuan_error_t getinfo_pid_cb(void *opaque, const void *buffer, size_t length)
{
    qint64 &pid = *static_cast<qint64 *>(opaque);
    pid = QByteArray(static_cast<const char *>(buffer), length).toLongLong();
    return 0;
}

static assuan_error_t command_data_cb(void *opaque, const void *buffer, size_t length)
{
    QByteArray &ba = *static_cast<QByteArray *>(opaque);
    ba.append(QByteArray(static_cast<const char *>(buffer), length));
    return 0;
}

namespace
{
struct inquire_data {
    const std::map<std::string, QByteArray> *map;
    const AssuanClientContext *ctx;
};
}

static assuan_error_t command_inquire_cb(void *opaque, const char *what)
{
    if (!opaque) {
        return 0;
    }
    const inquire_data &id = *static_cast<const inquire_data *>(opaque);
    const auto it = id.map->find(what);
    if (it != id.map->end()) {
        const QByteArray &v = it->second;
        assuan_send_data(id.ctx->get(), v.data(), v.size());
    }
    return 0;
}

static inline std::ostream &operator<<(std::ostream &s, const QByteArray &ba)
{
    return s << std::string(ba.data(), ba.size());
}

static assuan_error_t send_option(const AssuanClientContext &ctx, const char *name, const QVariant &value)
{
    std::stringstream ss;
    ss << "OPTION " << name;
    if (value.isValid()) {
        ss << '=' << value.toString().toUtf8();
    }
    return my_assuan_transact(ctx, ss.str().c_str());
}

static assuan_error_t send_file(const AssuanClientContext &ctx, const QString &file)
{
    std::stringstream ss;
    ss << "FILE " << hexencode(QFile::encodeName(file));
    return my_assuan_transact(ctx, ss.str().c_str());
}

static assuan_error_t send_recipient(const AssuanClientContext &ctx, const QString &recipient, bool info)
{
    std::stringstream ss;
    ss << "RECIPIENT ";
    if (info) {
        ss << "--info ";
    }
    ss << "--" << hexencode(recipient.toUtf8());
    return my_assuan_transact(ctx, ss.str().c_str());
}

static assuan_error_t send_sender(const AssuanClientContext &ctx, const QString &sender, bool info)
{
    std::stringstream ss;
    ss << "SENDER ";
    if (info) {
        ss << "--info ";
    }
    ss << "--" << hexencode(sender.toUtf8());
    return my_assuan_transact(ctx, ss.str().c_str());
}

void Command::Private::run()
{

    // Take a snapshot of the input data, and clear the output data:
    Inputs in;
    Outputs out;
    {
        const QMutexLocker locker(&mutex);
        in = inputs;
        outputs = out;
    }

    out.canceled = false;

    if (out.serverLocation.isEmpty()) {
        out.serverLocation = default_socket_name();
    }

#ifndef HAVE_ASSUAN2
    assuan_context_t naked_ctx = 0;
#endif
    AssuanClientContext ctx;
    assuan_error_t err = 0;

    inquire_data id = { &in.inquireData, &ctx };

    const QString socketName = out.serverLocation;
    if (socketName.isEmpty()) {
        out.errorString = i18n("Invalid socket name!");
        goto leave;
    }

#ifndef HAVE_ASSUAN2
    err = assuan_socket_connect(&naked_ctx, QFile::encodeName(socketName).constData(), -1);
#else
    {
        assuan_context_t naked_ctx = nullptr;
        err = assuan_new(&naked_ctx);
        if (err) {
            out.errorString = i18n("Could not allocate resources to connect to Kleopatra UI server at %1: %2"
                                   , socketName, to_error_string(err));
            goto leave;
        }

        ctx.reset(naked_ctx);
    }

    err = assuan_socket_connect(ctx.get(), QFile::encodeName(socketName).constData(), -1, 0);
#endif
    if (err) {
        qDebug("UI server not running, starting it");

        const QString errorString = start_uiserver();
        if (!errorString.isEmpty()) {
            out.errorString = errorString;
            goto leave;
        }

        // give it a bit of time to start up and try a couple of times
        for (int i = 0; err && i < 20; ++i) {
            msleep(500);
            err = assuan_socket_connect(ctx.get(), socketName.toUtf8().constData(), -1, 0);
        }
    }

    if (err) {
        out.errorString = i18n("Could not connect to Kleopatra UI server at %1: %2",
                               socketName, to_error_string(err));
        goto leave;
    }

#ifndef HAVE_ASSUAN2
    ctx.reset(naked_ctx);
    naked_ctx = 0;
#endif

    out.serverPid = -1;
    err = my_assuan_transact(ctx, "GETINFO pid", &getinfo_pid_cb, &out.serverPid);
    if (err || out.serverPid <= 0) {
        out.errorString = i18n("Could not get the process-id of the Kleopatra UI server at %1: %2", socketName, to_error_string(err));
        goto leave;
    }

    qCDebug(LIBKLEOPATRACLIENTCORE_LOG) << "Server PID =" << out.serverPid;

#if defined(Q_OS_WIN)
    if (!AllowSetForegroundWindow((pid_t)out.serverPid)) {
        qCDebug(LIBKLEOPATRACLIENTCORE_LOG) << "AllowSetForegroundWindow(" << out.serverPid << ") failed: " << GetLastError();
    }
#endif

    if (in.command.isEmpty()) {
        goto leave;
    }

    if (in.parentWId) {
#if defined(Q_OS_WIN32)
        err = send_option(ctx, "window-id", QString::asprintf("%lx", reinterpret_cast<quintptr>(in.parentWId)));
#else
        err = send_option(ctx, "window-id", QString::asprintf("%lx", static_cast<unsigned long>(in.parentWId)));
#endif
        if (err) {
            qDebug("sending option window-id failed - ignoring");
        }
    }

    for (auto it = in.options.begin(), end = in.options.end(); it != end; ++it)
        if ((err = send_option(ctx, it->first.c_str(), it->second.hasValue ? it->second.value.toString() : QVariant()))) {
            if (it->second.isCritical) {
                out.errorString = i18n("Failed to send critical option %1: %2", QString::fromLatin1(it->first.c_str()), to_error_string(err));
                goto leave;
            } else {
                qCDebug(LIBKLEOPATRACLIENTCORE_LOG) << "Failed to send non-critical option" << it->first.c_str() << ":" << to_error_string(err);
            }
        }

    Q_FOREACH (const QString &filePath, in.filePaths)
        if ((err = send_file(ctx, filePath))) {
            out.errorString = i18n("Failed to send file path %1: %2", filePath, to_error_string(err));
            goto leave;
        }

    Q_FOREACH (const QString &sender, in.senders)
        if ((err = send_sender(ctx, sender, in.areSendersInformative))) {
            out.errorString = i18n("Failed to send sender %1: %2", sender, to_error_string(err));
            goto leave;
        }

    Q_FOREACH (const QString &recipient, in.recipients)
        if ((err = send_recipient(ctx, recipient, in.areRecipientsInformative))) {
            out.errorString = i18n("Failed to send recipient %1: %2", recipient, to_error_string(err));
            goto leave;
        }

#if 0
    setup I / O;
#endif

    err = my_assuan_transact(ctx, in.command.constData(), &command_data_cb, &out.data, &command_inquire_cb, &id);
    if (err) {
        if (gpg_err_code(err) == GPG_ERR_CANCELED) {
            out.canceled = true;
        } else {
            out.errorString = i18n("Command (%1) failed: %2", QString::fromLatin1(in.command.constData()), to_error_string(err));
        }
        goto leave;
    }

leave:
    const QMutexLocker locker(&mutex);
    // copy outputs to where Command can see them:
    outputs = out;
}
