/* -*- mode: c++; c-basic-offset:4 -*-
    commands/adduseridcommand.cpp

    This file is part of Kleopatra, the KDE keymanager
    SPDX-FileCopyrightText: 2008 Klarälvdalens Datakonsult AB

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <config-kleopatra.h>

#include "adduseridcommand.h"

#include "command_p.h"

#include "dialogs/adduseriddialog.h"
#include "dialogs/addemaildialog.h"

#include <Libkleo/Formatting>
#include <QGpgME/Protocol>
#include <QGpgME/AddUserIDJob>

#include <gpgme++/key.h>

#include <KLocalizedString>
#include "kleopatra_debug.h"


using namespace Kleo;
using namespace Kleo::Commands;
using namespace Kleo::Dialogs;
using namespace GpgME;

class AddUserIDCommand::Private : public Command::Private
{
    friend class ::Kleo::Commands::AddUserIDCommand;
    AddUserIDCommand *q_func() const
    {
        return static_cast<AddUserIDCommand *>(q);
    }
public:
    explicit Private(AddUserIDCommand *qq, KeyListController *c);
    ~Private() override;

    void init();

private:
    void slotDialogAccepted();
    void slotSimpleDialogAccepted();
    void slotDialogRejected();
    void slotResult(const Error &err);

private:
    void ensureDialogCreated();
    void createJob();
    void showErrorDialog(const Error &error);
    void showSuccessDialog();

private:
    GpgME::Key key;
    QPointer<AddUserIDDialog> dialog;
    QPointer<AddEmailDialog> simpleDialog;
    QPointer<QGpgME::AddUserIDJob> job;
};

AddUserIDCommand::Private *AddUserIDCommand::d_func()
{
    return static_cast<Private *>(d.get());
}
const AddUserIDCommand::Private *AddUserIDCommand::d_func() const
{
    return static_cast<const Private *>(d.get());
}

#define d d_func()
#define q q_func()

AddUserIDCommand::Private::Private(AddUserIDCommand *qq, KeyListController *c)
    : Command::Private(qq, c),
      key(),
      dialog(),
      job()
{

}

AddUserIDCommand::Private::~Private()
{
    qCDebug(KLEOPATRA_LOG);
    if (dialog) {
        delete dialog;
    }
    if (simpleDialog) {
        delete simpleDialog;
    }
}

AddUserIDCommand::AddUserIDCommand(KeyListController *c)
    : Command(new Private(this, c))
{
    d->init();
}

AddUserIDCommand::AddUserIDCommand(QAbstractItemView *v, KeyListController *c)
    : Command(v, new Private(this, c))
{
    d->init();
}

AddUserIDCommand::AddUserIDCommand(const GpgME::Key &key)
    : Command(key, new Private(this, nullptr))
{
    d->init();
}

void AddUserIDCommand::Private::init()
{

}

AddUserIDCommand::~AddUserIDCommand()
{
    qCDebug(KLEOPATRA_LOG);
}

void AddUserIDCommand::doStart()
{

    const std::vector<Key> keys = d->keys();
    if (keys.size() != 1 ||
            keys.front().protocol() != GpgME::OpenPGP ||
            !keys.front().hasSecret()) {
        d->finished();
        return;
    }

    d->key = keys.front();

    d->ensureDialogCreated();
    Q_ASSERT(d->dialog);

    const UserID uid = d->key.userID(0);

    // d->simpleDialog->setEmail(Formatting::prettyEMail(uid.email(), uid.id()));

    d->dialog->setName(QString::fromUtf8(uid.name()));
    d->dialog->setEmail(Formatting::prettyEMail(uid.email(), uid.id()));
    d->dialog->setComment(QString::fromUtf8(uid.comment()));

    d->simpleDialog->show();
}

void AddUserIDCommand::Private::slotSimpleDialogAccepted()
{
    if (simpleDialog->advancedSelected()) {
        qDebug() << "thinking advanced selected";
        dialog->show();
        return;
    }

    createJob();
    if (!job) {
        finished();
        return;
    }
    if (const Error err = job->start(key, QString(), simpleDialog->email(), QString())) {
        showErrorDialog(err);
        finished();
    }
}

void AddUserIDCommand::Private::slotDialogAccepted()
{
    Q_ASSERT(dialog);

    createJob();
    if (!job) {
        finished();
    }

    else if (const Error err = job->start(key, dialog->name(), dialog->email(), dialog->comment())) {
        showErrorDialog(err);
        finished();
    }
}

void AddUserIDCommand::Private::slotDialogRejected()
{
    Q_EMIT q->canceled();
    finished();
}

void AddUserIDCommand::Private::slotResult(const Error &err)
{
    if (err.isCanceled())
        ;
    else if (err) {
        showErrorDialog(err);
    } else {
        showSuccessDialog();
    }
    finished();
}

void AddUserIDCommand::doCancel()
{
    qCDebug(KLEOPATRA_LOG);
    if (d->job) {
        d->job->slotCancel();
    }
}

void AddUserIDCommand::Private::ensureDialogCreated()
{
    if (dialog) {
        return;
    }

    dialog = new AddUserIDDialog;
    applyWindowID(dialog);

    connect(dialog, SIGNAL(accepted()), q, SLOT(slotDialogAccepted()));
    connect(dialog, SIGNAL(rejected()), q, SLOT(slotDialogRejected()));

    simpleDialog = new AddEmailDialog;
    applyWindowID(simpleDialog);

    connect(simpleDialog, SIGNAL(accepted()), q, SLOT(slotSimpleDialogAccepted()));
    connect(simpleDialog, SIGNAL(rejected()), q, SLOT(slotDialogRejected()));
}

void AddUserIDCommand::Private::createJob()
{
    Q_ASSERT(!job);

    const auto backend = (key.protocol() == GpgME::OpenPGP) ? QGpgME::openpgp() : QGpgME::smime();
    if (!backend) {
        return;
    }

    QGpgME::AddUserIDJob *const j = backend->addUserIDJob();
    if (!j) {
        return;
    }

    connect(j, &QGpgME::Job::progress,
            q, &Command::progress);
    connect(j, SIGNAL(result(GpgME::Error)),
            q, SLOT(slotResult(GpgME::Error)));

    job = j;
}

void AddUserIDCommand::Private::showErrorDialog(const Error &err)
{
    error(xi18nc("@info",
                 "<para>An error occurred while trying to add the user-id: "
                 "<message>%1</message></para>",
                 QString::fromLocal8Bit(err.asString())),
          i18nc("@title:window", "Add User-ID Error"));
}

void AddUserIDCommand::Private::showSuccessDialog()
{
    information(i18nc("@info", "User-ID successfully added."),
                i18nc("@title:window", "Add User-ID Succeeded"));
}

#undef d
#undef q

#include "moc_adduseridcommand.cpp"
