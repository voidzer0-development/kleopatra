/* -*- mode: c++; c-basic-offset:4 -*-
    commands/dumpcrlcachecommand.cpp

    This file is part of Kleopatra, the KDE keymanager
    SPDX-FileCopyrightText: 2008 Klarälvdalens Datakonsult AB

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <config-kleopatra.h>

#include "dumpcrlcachecommand.h"

#include "command_p.h"

#include <Libkleo/GnuPG>

#include <KProcess>
#include <KMessageBox>
#include <KLocalizedString>
#include <QPushButton>
#include <KStandardGuiItem>
#include <KConfigGroup>

#include <QString>
#include <QByteArray>
#include <QTimer>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <KSharedConfig>
#include <QFontDatabase>
#include <QTextEdit>

static const int PROCESS_TERMINATE_TIMEOUT = 5000; // milliseconds

namespace
{
class DumpCrlCacheDialog : public QDialog
{
    Q_OBJECT
public:
    explicit DumpCrlCacheDialog(QWidget *parent = nullptr)
        : QDialog(parent), ui(this), mWithRevocations(false)
    {
        readConfig();
    }
    ~DumpCrlCacheDialog() override
    {
        writeConfig();
    }

Q_SIGNALS:
    void updateRequested();

public Q_SLOTS:
    void append(const QString &line)
    {
        ui.logTextWidget.append(line);
        ui.logTextWidget.ensureCursorVisible();
    }
    void clear()
    {
        ui.logTextWidget.clear();
    }

public:
    void setWithRevocations (bool value) {
        mWithRevocations = value;
    }

    Q_REQUIRED_RESULT bool withRevocations () {
        return mWithRevocations;
    }

private:
    void readConfig()
    {
        KConfigGroup dialog(KSharedConfig::openStateConfig(), "DumpCrlCacheDialog");
        const QSize size = dialog.readEntry("Size", QSize(600, 400));
        if (size.isValid()) {
            resize(size);
        }
    }

    void writeConfig()
    {
        KConfigGroup dialog(KSharedConfig::openStateConfig(), "DumpCrlCacheDialog");
        dialog.writeEntry("Size", size());
        dialog.sync();
    }

    struct Ui {
        QTextEdit   logTextWidget;
        QPushButton     updateButton, closeButton, revocationsButton;
        QVBoxLayout vlay;
        QHBoxLayout  hlay;

        explicit Ui(DumpCrlCacheDialog *q)
            : logTextWidget(q),
              updateButton(i18nc("@action:button Update the log text widget", "&Update"), q),
              closeButton(q),
              vlay(q),
              hlay()
        {
            KGuiItem::assign(&closeButton, KStandardGuiItem::close());
            KDAB_SET_OBJECT_NAME(logTextWidget);
            KDAB_SET_OBJECT_NAME(updateButton);
            KDAB_SET_OBJECT_NAME(closeButton);
            KDAB_SET_OBJECT_NAME(vlay);
            KDAB_SET_OBJECT_NAME(hlay);

            logTextWidget.setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
            logTextWidget.setReadOnly(true);

            vlay.addWidget(&logTextWidget, 1);
            vlay.addLayout(&hlay);

            revocationsButton.setText(i18n("Show Entries"));

            hlay.addWidget(&updateButton);
            hlay.addWidget(&revocationsButton);
            hlay.addStretch(1);
            hlay.addWidget(&closeButton);

            connect(&updateButton, &QAbstractButton::clicked,
                    q, &DumpCrlCacheDialog::updateRequested);
            connect(&closeButton, &QAbstractButton::clicked,
                    q, &QWidget::close);

            connect(&revocationsButton, &QAbstractButton::clicked,
                    q, [q, this] () {
                q->mWithRevocations = true;
                revocationsButton.setEnabled(false);
                q->updateRequested();
            });
        }
    } ui;
    bool mWithRevocations;
};
}

using namespace Kleo;
using namespace Kleo::Commands;

static QByteArray chomped(QByteArray ba)
{
    while (ba.endsWith('\n') || ba.endsWith('\r')) {
        ba.chop(1);
    }
    return ba;
}

class DumpCrlCacheCommand::Private : Command::Private
{
    friend class ::Kleo::Commands::DumpCrlCacheCommand;
    DumpCrlCacheCommand *q_func() const
    {
        return static_cast<DumpCrlCacheCommand *>(q);
    }
public:
    explicit Private(DumpCrlCacheCommand *qq, KeyListController *c);
    ~Private() override;

    QString errorString() const
    {
        return QString::fromLocal8Bit(errorBuffer);
    }

private:
    void init();
    void refreshView();

private:
    void slotProcessFinished(int, QProcess::ExitStatus);

    void slotProcessReadyReadStandardOutput()
    {
        static int count;
        while (process.canReadLine()) {
            if (!dialog) {
                break;
            }
            const QByteArray line = chomped(process.readLine());
            if (!line.size()) {
                continue;
            }
            if (!dialog->withRevocations() && line.contains("reasons")) {
                count++;
                continue;
            } else if (count) {
                dialog->append (QLatin1Char(' ') +
                    i18nc("Count of revocations in a CRL",
                          "Entries:") + QStringLiteral("\t\t%1\n").arg(count));
                count = 0;
            }
            dialog->append(stringFromGpgOutput(line));
        }
    }

    void slotProcessReadyReadStandardError()
    {
        errorBuffer += process.readAllStandardError();
    }

    void slotUpdateRequested()
    {
        if (process.state() == QProcess::NotRunning) {
            refreshView();
        }
    }

    void slotDialogDestroyed()
    {
        dialog = nullptr;
        if (process.state() != QProcess::NotRunning) {
            q->cancel();
        } else {
            finished();
        }
    }

private:
    DumpCrlCacheDialog *dialog;
    KProcess process;
    QByteArray errorBuffer;
    bool canceled;
};

DumpCrlCacheCommand::Private *DumpCrlCacheCommand::d_func()
{
    return static_cast<Private *>(d.get());
}
const DumpCrlCacheCommand::Private *DumpCrlCacheCommand::d_func() const
{
    return static_cast<const Private *>(d.get());
}

#define d d_func()
#define q q_func()

DumpCrlCacheCommand::Private::Private(DumpCrlCacheCommand *qq, KeyListController *c)
    : Command::Private(qq, c),
      dialog(nullptr),
      process(),
      errorBuffer(),
      canceled(false)
{
    process.setOutputChannelMode(KProcess::SeparateChannels);
    process.setReadChannel(KProcess::StandardOutput);
    process << gpgSmPath() << QStringLiteral("--call-dirmngr") << QStringLiteral("listcrls");
}

DumpCrlCacheCommand::Private::~Private()
{
    if (dialog && !dialog->isVisible()) {
        delete dialog;
    }
}

DumpCrlCacheCommand::DumpCrlCacheCommand(KeyListController *c)
    : Command(new Private(this, c))
{
    d->init();
}

DumpCrlCacheCommand::DumpCrlCacheCommand(QAbstractItemView *v, KeyListController *c)
    : Command(v, new Private(this, c))
{
    d->init();
}

void DumpCrlCacheCommand::Private::init()
{
    connect(&process, SIGNAL(finished(int,QProcess::ExitStatus)),
            q, SLOT(slotProcessFinished(int,QProcess::ExitStatus)));
    connect(&process, SIGNAL(readyReadStandardError()),
            q, SLOT(slotProcessReadyReadStandardError()));
    connect(&process, SIGNAL(readyReadStandardOutput()),
            q, SLOT(slotProcessReadyReadStandardOutput()));
}

DumpCrlCacheCommand::~DumpCrlCacheCommand() {}

void DumpCrlCacheCommand::doStart()
{

    d->dialog = new DumpCrlCacheDialog;
    d->dialog->setAttribute(Qt::WA_DeleteOnClose);
    d->dialog->setWindowTitle(i18nc("@title:window", "CRL Cache Dump"));

    connect(d->dialog, SIGNAL(updateRequested()),
            this, SLOT(slotUpdateRequested()));
    connect(d->dialog, SIGNAL(destroyed()),
            this, SLOT(slotDialogDestroyed()));

    d->refreshView();
}

void DumpCrlCacheCommand::Private::refreshView()
{

    dialog->clear();

    process.start();

    if (process.waitForStarted()) {
        dialog->show();
    } else {
        KMessageBox::error(dialog ? static_cast<QWidget *>(dialog) : parentWidgetOrView(),
                           i18n("Unable to start process gpgsm. "
                                "Please check your installation."),
                           i18n("Dump CRL Cache Error"));
        finished();
    }
}

void DumpCrlCacheCommand::doCancel()
{
    d->canceled = true;
    if (d->process.state() != QProcess::NotRunning) {
        d->process.terminate();
        QTimer::singleShot(PROCESS_TERMINATE_TIMEOUT, &d->process, &QProcess::kill);
    }
    if (d->dialog) {
        d->dialog->close();
    }
    d->dialog = nullptr;
}

void DumpCrlCacheCommand::Private::slotProcessFinished(int code, QProcess::ExitStatus status)
{
    if (!canceled) {
        if (status == QProcess::CrashExit)
            KMessageBox::error(dialog,
                               i18n("The GpgSM process that tried to dump the CRL cache "
                                    "ended prematurely because of an unexpected error. "
                                    "Please check the output of gpgsm --call-dirmngr listcrls for details."),
                               i18n("Dump CRL Cache Error"));
        else if (code)
            KMessageBox::error(dialog,
                               i18n("An error occurred while trying to dump the CRL cache. "
                                    "The output from GpgSM was:\n%1", errorString()),
                               i18n("Dump CRL Cache Error"));
    }
}

#undef d
#undef q

#include "moc_dumpcrlcachecommand.cpp"
#include "dumpcrlcachecommand.moc"
