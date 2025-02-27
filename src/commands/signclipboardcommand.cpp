/* -*- mode: c++; c-basic-offset:4 -*-
    commands/signclipboardcommand.cpp

    This file is part of Kleopatra, the KDE keymanager
    SPDX-FileCopyrightText: 2008 Klarälvdalens Datakonsult AB

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <config-kleopatra.h>

#include "signclipboardcommand.h"

#ifndef QT_NO_CLIPBOARD

#include "command_p.h"

#include <crypto/signemailcontroller.h>

#include <utils/input.h>
#include <utils/output.h>

#include <Libkleo/Stl_Util>

#include <KLocalizedString>
#include "kleopatra_debug.h"

#include <QApplication>
#include <QClipboard>
#include <QMimeData>

#include <exception>

using namespace Kleo;
using namespace Kleo::Commands;
using namespace Kleo::Crypto;

class SignClipboardCommand::Private : public Command::Private
{
    friend class ::Kleo::Commands::SignClipboardCommand;
    SignClipboardCommand *q_func() const
    {
        return static_cast<SignClipboardCommand *>(q);
    }
public:
    explicit Private(SignClipboardCommand *qq, KeyListController *c);
    ~Private() override;

    void init();

private:
    void slotSignersResolved();
    void slotControllerDone()
    {
        finished();
    }
    void slotControllerError(int, const QString &)
    {
        finished();
    }

private:
    std::shared_ptr<const ExecutionContext> shared_qq;
    std::shared_ptr<Input> input;
    SignEMailController controller;
};

SignClipboardCommand::Private *SignClipboardCommand::d_func()
{
    return static_cast<Private *>(d.get());
}
const SignClipboardCommand::Private *SignClipboardCommand::d_func() const
{
    return static_cast<const Private *>(d.get());
}

#define d d_func()
#define q q_func()

SignClipboardCommand::Private::Private(SignClipboardCommand *qq, KeyListController *c)
    : Command::Private(qq, c),
      shared_qq(qq, [](SignClipboardCommand*){}),
      input(),
      controller(SignEMailController::ClipboardMode)
{

}

SignClipboardCommand::Private::~Private()
{
    qCDebug(KLEOPATRA_LOG);
}

SignClipboardCommand::SignClipboardCommand(GpgME::Protocol protocol, KeyListController *c)
    : Command(new Private(this, c))
{
    d->init();
    d->controller.setProtocol(protocol);
}

SignClipboardCommand::SignClipboardCommand(GpgME::Protocol protocol, QAbstractItemView *v, KeyListController *c)
    : Command(v, new Private(this, c))
{
    d->init();
    d->controller.setProtocol(protocol);
}

void SignClipboardCommand::Private::init()
{
    controller.setExecutionContext(shared_qq);
    controller.setDetachedSignature(false);
    connect(&controller, SIGNAL(done()), q, SLOT(slotControllerDone()));
    connect(&controller, SIGNAL(error(int,QString)), q, SLOT(slotControllerError(int,QString)));
}

SignClipboardCommand::~SignClipboardCommand()
{
    qCDebug(KLEOPATRA_LOG);
}

// static
bool SignClipboardCommand::canSignCurrentClipboard()
{
    if (const QClipboard *clip = QApplication::clipboard())
        if (const QMimeData *mime = clip->mimeData()) {
            return mime->hasText();
        }
    return false;
}

void SignClipboardCommand::doStart()
{

    try {

        // snapshot clipboard content here, in case it's being changed...
        d->input = Input::createFromClipboard();

        connect(&d->controller, SIGNAL(signersResolved()),
                this, SLOT(slotSignersResolved()));

        d->controller.startResolveSigners();

    } catch (const std::exception &e) {
        d->information(i18n("An error occurred: %1",
                            QString::fromLocal8Bit(e.what())),
                       i18n("Sign Clipboard Error"));
        d->finished();
    }
}

void SignClipboardCommand::Private::slotSignersResolved()
{
    try {
        controller.setInputAndOutput(input, Output::createFromClipboard());
        input.reset(); // no longer needed, so don't keep a reference
        controller.start();
    } catch (const std::exception &e) {
        information(i18n("An error occurred: %1",
                         QString::fromLocal8Bit(e.what())),
                    i18n("Sign Clipboard Error"));
        finished();
    }
}

void SignClipboardCommand::doCancel()
{
    qCDebug(KLEOPATRA_LOG);
    d->controller.cancel();
}

#undef d
#undef q

#include "moc_signclipboardcommand.cpp"

#endif // QT_NO_CLIPBOARD
