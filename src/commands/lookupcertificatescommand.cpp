/* -*- mode: c++; c-basic-offset:4 -*-
    commands/lookupcertificatescommand.cpp

    This file is part of Kleopatra, the KDE keymanager
    SPDX-FileCopyrightText: 2008, 2009 Klarälvdalens Datakonsult AB

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <config-kleopatra.h>

#include "lookupcertificatescommand.h"

#include "importcertificatescommand_p.h"

#include "detailscommand.h"

#include <Libkleo/Compat>
#include <Libkleo/GnuPG>

#include <dialogs/lookupcertificatesdialog.h>

#include <Libkleo/Formatting>
#include <Libkleo/Stl_Util>

#include <QGpgME/CryptoConfig>
#include <QGpgME/Protocol>
#include <QGpgME/KeyListJob>
#include <QGpgME/ImportFromKeyserverJob>

#include <gpgme++/key.h>
#include <gpgme++/keylistresult.h>
#include <gpgme++/importresult.h>

#include <KLocalizedString>
#include <KMessageBox>
#include "kleopatra_debug.h"

#include <QRegExp>

#include <vector>
#include <map>
#include <algorithm>

using namespace Kleo;
using namespace Kleo::Commands;
using namespace Kleo::Dialogs;
using namespace GpgME;
using namespace QGpgME;

class LookupCertificatesCommand::Private : public ImportCertificatesCommand::Private
{
    friend class ::Kleo::Commands::LookupCertificatesCommand;
    LookupCertificatesCommand *q_func() const
    {
        return static_cast<LookupCertificatesCommand *>(q);
    }
public:
    explicit Private(LookupCertificatesCommand *qq, KeyListController *c);
    ~Private() override;

    QString query;
    void init();

private:
    void slotSearchTextChanged(const QString &str);
    void slotNextKey(const Key &key)
    {
        keyListing.keys.push_back(key);
    }
    void slotKeyListResult(const KeyListResult &result);
    void slotImportRequested(const std::vector<Key> &keys);
    void slotDetailsRequested(const Key &key);
    void slotSaveAsRequested(const std::vector<Key> &keys);
    void slotDialogRejected()
    {
        canceled();
    }

private:
    using ImportCertificatesCommand::Private::showError;
    void showError(QWidget *parent, const KeyListResult &result);
    void showResult(QWidget *parent, const KeyListResult &result);
    void createDialog();
    KeyListJob *createKeyListJob(GpgME::Protocol proto) const
    {
        const auto cbp = (proto == GpgME::OpenPGP) ? QGpgME::openpgp() : QGpgME::smime();
        return cbp ? cbp->keyListJob(true) : nullptr;
    }
    ImportFromKeyserverJob *createImportJob(GpgME::Protocol proto) const
    {
        const auto cbp = (proto == GpgME::OpenPGP) ? QGpgME::openpgp() : QGpgME::smime();
        return cbp ? cbp->importFromKeyserverJob() : nullptr;
    }
    void startKeyListJob(GpgME::Protocol proto, const QString &str);
    bool checkConfig() const;

    QWidget *dialogOrParentWidgetOrView() const
    {
        if (dialog) {
            return dialog;
        } else {
            return parentWidgetOrView();
        }
    }

private:
    QPointer<LookupCertificatesDialog> dialog;
    struct KeyListingVariables {
        QPointer<KeyListJob> cms, openpgp;
        KeyListResult result;
        std::vector<Key> keys;

        void reset()
        {
            *this = KeyListingVariables();
        }
    } keyListing;
};

LookupCertificatesCommand::Private *LookupCertificatesCommand::d_func()
{
    return static_cast<Private *>(d.get());
}
const LookupCertificatesCommand::Private *LookupCertificatesCommand::d_func() const
{
    return static_cast<const Private *>(d.get());
}

#define d d_func()
#define q q_func()

LookupCertificatesCommand::Private::Private(LookupCertificatesCommand *qq, KeyListController *c)
    : ImportCertificatesCommand::Private(qq, c),
      dialog()
{

}

LookupCertificatesCommand::Private::~Private()
{
    qCDebug(KLEOPATRA_LOG);
    delete dialog;
}

LookupCertificatesCommand::LookupCertificatesCommand(KeyListController *c)
    : ImportCertificatesCommand(new Private(this, c))
{
    d->init();
}

LookupCertificatesCommand::LookupCertificatesCommand(const QString &query, KeyListController *c)
    : ImportCertificatesCommand(new Private(this, c))
{
    d->init();
    d->query = query;
}

LookupCertificatesCommand::LookupCertificatesCommand(QAbstractItemView *v, KeyListController *c)
    : ImportCertificatesCommand(v, new Private(this, c))
{
    d->init();
}

void LookupCertificatesCommand::Private::init()
{

}

LookupCertificatesCommand::~LookupCertificatesCommand()
{
    qCDebug(KLEOPATRA_LOG);
}

void LookupCertificatesCommand::doStart()
{

    if (!d->checkConfig()) {
        d->finished();
        return;
    }

    d->createDialog();
    Q_ASSERT(d->dialog);

    // if we have a prespecified query, load it into find field
    // and start the search
    if (!d->query.isEmpty()) {
        d->dialog->setSearchText(d->query);
        d->slotSearchTextChanged(d->query);
    } else {
        d->dialog->setPassive(false);
    }

    d->dialog->show();

}

void LookupCertificatesCommand::Private::createDialog()
{
    if (dialog) {
        return;
    }
    dialog = new LookupCertificatesDialog;
    applyWindowID(dialog);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, SIGNAL(searchTextChanged(QString)),
            q, SLOT(slotSearchTextChanged(QString)));
    connect(dialog, SIGNAL(saveAsRequested(std::vector<GpgME::Key>)),
            q, SLOT(slotSaveAsRequested(std::vector<GpgME::Key>)));
    connect(dialog, SIGNAL(importRequested(std::vector<GpgME::Key>)),
            q, SLOT(slotImportRequested(std::vector<GpgME::Key>)));
    connect(dialog, SIGNAL(detailsRequested(GpgME::Key)),
            q, SLOT(slotDetailsRequested(GpgME::Key)));
    connect(dialog, SIGNAL(rejected()),
            q, SLOT(slotDialogRejected()));
}

void LookupCertificatesCommand::Private::slotSearchTextChanged(const QString &str)
{
    // pressing return might trigger both search and dialog destruction (search focused and default key set)
    // On Windows, the dialog is then destroyed before this slot is called
    if (dialog) {   //thus test
        dialog->setPassive(true);
        dialog->setCertificates(std::vector<Key>());
    }

    query = str;

    startKeyListJob(CMS,     str);

    const QRegExp rx(QLatin1String("(?:0x|0X)?[0-9a-fA-F]{6,}"));
    if (rx.exactMatch(query) && !str.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        qCDebug(KLEOPATRA_LOG) << "Adding 0x prefix to query";
        startKeyListJob(OpenPGP, QStringLiteral("0x") + str);
    } else {
        startKeyListJob(OpenPGP, str);
    }
}

void LookupCertificatesCommand::Private::startKeyListJob(GpgME::Protocol proto, const QString &str)
{
    KeyListJob *const klj = createKeyListJob(proto);
    if (!klj) {
        return;
    }
    connect(klj, SIGNAL(result(GpgME::KeyListResult)),
            q, SLOT(slotKeyListResult(GpgME::KeyListResult)));
    connect(klj, SIGNAL(nextKey(GpgME::Key)),
            q, SLOT(slotNextKey(GpgME::Key)));
    if (const Error err = klj->start(QStringList(str))) {
        keyListing.result.mergeWith(KeyListResult(err));
    } else if (proto == CMS) {
        keyListing.cms     = klj;
    } else {
        keyListing.openpgp = klj;
    }
}

void LookupCertificatesCommand::Private::slotKeyListResult(const KeyListResult &r)
{

    if (q->sender() == keyListing.cms) {
        keyListing.cms = nullptr;
    } else if (q->sender() == keyListing.openpgp) {
        keyListing.openpgp = nullptr;
    } else {
        qCDebug(KLEOPATRA_LOG) << "unknown sender()" << q->sender();
    }

    keyListing.result.mergeWith(r);
    if (keyListing.cms || keyListing.openpgp) { // still waiting for jobs to complete
        return;
    }

    if (keyListing.result.error() && !keyListing.result.error().isCanceled()) {
        showError(dialog, keyListing.result);
    }

    if (keyListing.result.isTruncated()) {
        showResult(dialog, keyListing.result);
    }

    if (dialog) {
        dialog->setPassive(false);
        dialog->setCertificates(keyListing.keys);
    } else {
        finished();
    }

    keyListing.reset();
}

void LookupCertificatesCommand::Private::slotImportRequested(const std::vector<Key> &keys)
{
    dialog = nullptr;

    Q_ASSERT(!keys.empty());
    Q_ASSERT(std::none_of(keys.cbegin(), keys.cend(), [](const Key &key) { return key.isNull(); }));

    std::vector<Key> pgp, cms;
    pgp.reserve(keys.size());
    cms.reserve(keys.size());
    kdtools::separate_if(keys.begin(), keys.end(),
                         std::back_inserter(pgp),
                         std::back_inserter(cms),
                         [](const Key &key) {
                             return key.protocol() == GpgME::OpenPGP;
                         });

    setWaitForMoreJobs(true);
    if (!pgp.empty())
        startImport(OpenPGP, pgp,
                    i18nc(R"(@title %1:"OpenPGP" or "CMS")",
                          "%1 Certificate Server",
                          Formatting::displayName(OpenPGP)));
    if (!cms.empty())
        startImport(CMS, cms,
                    i18nc(R"(@title %1:"OpenPGP" or "CMS")",
                          "%1 Certificate Server",
                          Formatting::displayName(CMS)));
    setWaitForMoreJobs(false);
}

void LookupCertificatesCommand::Private::slotSaveAsRequested(const std::vector<Key> &keys)
{
    Q_UNUSED(keys)
    qCDebug(KLEOPATRA_LOG) << "not implemented";
}

void LookupCertificatesCommand::Private::slotDetailsRequested(const Key &key)
{
    Command *const cmd = new DetailsCommand(key, view(), controller());
    cmd->setParentWidget(dialogOrParentWidgetOrView());
    cmd->start();
}

void LookupCertificatesCommand::doCancel()
{
    ImportCertificatesCommand::doCancel();
    if (QDialog *const dlg = d->dialog) {
        d->dialog = nullptr;
        dlg->close();
    }
}

void LookupCertificatesCommand::Private::showError(QWidget *parent, const KeyListResult &result)
{
    if (!result.error()) {
        return;
    }
    KMessageBox::information(parent, i18nc("@info",
                                           "Failed to search on certificate server. The error returned was:\n%1",
                                           QString::fromLocal8Bit(result.error().asString())));
}

void LookupCertificatesCommand::Private::showResult(QWidget *parent, const KeyListResult &result)
{
    if (result.isTruncated())
        KMessageBox::information(parent,
                                 xi18nc("@info",
                                        "<para>The query result has been truncated.</para>"
                                        "<para>Either the local or a remote limit on "
                                        "the maximum number of returned hits has "
                                        "been exceeded.</para>"
                                        "<para>You can try to increase the local limit "
                                        "in the configuration dialog, but if one "
                                        "of the configured servers is the limiting "
                                        "factor, you have to refine your search.</para>"),
                                 i18nc("@title", "Result Truncated"),
                                 QStringLiteral("lookup-certificates-truncated-result"));
}

bool LookupCertificatesCommand::Private::checkConfig() const
{
    const bool ok = haveKeyserverConfigured() || haveX509DirectoryServerConfigured();
    if (!ok)
        information(xi18nc("@info",
                           "<para>You do not have any directory servers configured.</para>"
                           "<para>You need to configure at least one directory server to "
                           "search on one.</para>"
                           "<para>You can configure directory servers here: "
                           "<interface>Settings->Configure Kleopatra</interface>.</para>"),
                    i18nc("@title", "No Directory Servers Configured"));
    return ok;
}

#undef d
#undef q

#include "moc_lookupcertificatescommand.cpp"
