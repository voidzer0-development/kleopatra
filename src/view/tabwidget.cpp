/* -*- mode: c++; c-basic-offset:4 -*-
    view/tabwidget.cpp

    This file is part of Kleopatra, the KDE keymanager
    SPDX-FileCopyrightText: 2007 Klarälvdalens Datakonsult AB

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <config-kleopatra.h>

#include "tabwidget.h"
#include "keytreeview.h"
#include "kleopatra_debug.h"

#include <utils/action_data.h>

#include <Libkleo/Stl_Util>
#include <Libkleo/KeyFilter>
#include <Libkleo/KeyFilterManager>
#include <Libkleo/KeyListModel>
#include <Libkleo/KeyListSortFilterProxyModel>

#include <gpgme++/key.h>

#include <KLocalizedString>
#include <QTabWidget>
#include <KConfigGroup>
#include <KSharedConfig>
#include <KConfig>
#include <QAction>
#include <KActionCollection>
#include <QInputDialog>

#include <QTreeView>
#include <QToolButton>
#include <QMenu>
#include <QVBoxLayout>
#include <QRegularExpression>
#include <QAbstractProxyModel>

#include <map>

using namespace Kleo;
using namespace GpgME;

namespace
{

class Page : public Kleo::KeyTreeView
{
    Q_OBJECT
    Page(const Page &other);
public:
    Page(const QString &title, const QString &id, const QString &text, AbstractKeyListSortFilterProxyModel *proxy = nullptr, const QString &toolTip = QString(), QWidget *parent = nullptr, const KConfigGroup &group = KConfigGroup());
    Page(const KConfigGroup &group, QWidget *parent = nullptr);
    ~Page() override;

    void setTemporary(bool temporary);
    bool isTemporary() const
    {
        return m_isTemporary;
    }

    void setHierarchicalView(bool hierarchical) override;
    void setStringFilter(const QString &filter) override;
    void setKeyFilter(const std::shared_ptr<KeyFilter> &filter) override;

    QString title() const
    {
        return m_title.isEmpty() && keyFilter() ? keyFilter()->name() : m_title;
    }
    void setTitle(const QString &title);

    QString toolTip() const
    {
        return m_toolTip.isEmpty() ? title() : m_toolTip;
    }
    // not used void setToolTip(const QString &tip);

    bool canBeClosed() const
    {
        return m_canBeClosed;
    }
    bool canBeRenamed() const
    {
        return m_canBeRenamed;
    }
    bool canChangeStringFilter() const
    {
        return m_canChangeStringFilter;
    }
    bool canChangeKeyFilter() const
    {
        return m_canChangeKeyFilter && !m_isTemporary;
    }
    bool canChangeHierarchical() const
    {
        return m_canChangeHierarchical;
    }

    void saveTo(KConfigGroup &group) const;

    Page *clone() const override
    {
        return new Page(*this);
    }

    void liftAllRestrictions()
    {
        m_canBeClosed = m_canBeRenamed = m_canChangeStringFilter = m_canChangeKeyFilter = m_canChangeHierarchical = true;
    }

Q_SIGNALS:
    void titleChanged(const QString &title);

private:
    void init();

private:
    QString m_title;
    QString m_toolTip;
    bool m_isTemporary : 1;
    bool m_canBeClosed : 1;
    bool m_canBeRenamed : 1;
    bool m_canChangeStringFilter : 1;
    bool m_canChangeKeyFilter : 1;
    bool m_canChangeHierarchical : 1;
};
} // anon namespace

Page::Page(const Page &other)
    : KeyTreeView(other),
      m_title(other.m_title),
      m_toolTip(other.m_toolTip),
      m_isTemporary(other.m_isTemporary),
      m_canBeClosed(other.m_canBeClosed),
      m_canBeRenamed(other.m_canBeRenamed),
      m_canChangeStringFilter(other.m_canChangeStringFilter),
      m_canChangeKeyFilter(other.m_canChangeKeyFilter),
      m_canChangeHierarchical(other.m_canChangeHierarchical)
{
    init();
}

Page::Page(const QString &title, const QString &id, const QString &text, AbstractKeyListSortFilterProxyModel *proxy, const QString &toolTip, QWidget *parent,
           const KConfigGroup &group)
    : KeyTreeView(text, KeyFilterManager::instance()->keyFilterByID(id), proxy, parent, group),
      m_title(title),
      m_toolTip(toolTip),
      m_isTemporary(false),
      m_canBeClosed(true),
      m_canBeRenamed(true),
      m_canChangeStringFilter(true),
      m_canChangeKeyFilter(true),
      m_canChangeHierarchical(true)
{
    init();
}

static const char TITLE_ENTRY[] = "title";
static const char STRING_FILTER_ENTRY[] = "string-filter";
static const char KEY_FILTER_ENTRY[] = "key-filter";
static const char HIERARCHICAL_VIEW_ENTRY[] = "hierarchical-view";
static const char COLUMN_SIZES[] = "column-sizes";
static const char SORT_COLUMN[] = "sort-column";
static const char SORT_DESCENDING[] = "sort-descending";

Page::Page(const KConfigGroup &group, QWidget *parent)
    : KeyTreeView(group.readEntry(STRING_FILTER_ENTRY),
                  KeyFilterManager::instance()->keyFilterByID(group.readEntry(KEY_FILTER_ENTRY)),
                  nullptr, parent, group),
      m_title(group.readEntry(TITLE_ENTRY)),
      m_toolTip(),
      m_isTemporary(false),
      m_canBeClosed(!group.isImmutable()),
      m_canBeRenamed(!group.isEntryImmutable(TITLE_ENTRY)),
      m_canChangeStringFilter(!group.isEntryImmutable(STRING_FILTER_ENTRY)),
      m_canChangeKeyFilter(!group.isEntryImmutable(KEY_FILTER_ENTRY)),
      m_canChangeHierarchical(!group.isEntryImmutable(HIERARCHICAL_VIEW_ENTRY))
{
    init();
    setHierarchicalView(group.readEntry(HIERARCHICAL_VIEW_ENTRY, true));
    const QList<int> settings = group.readEntry(COLUMN_SIZES, QList<int>());
    std::vector<int> sizes;
    sizes.reserve(settings.size());
    std::copy(settings.cbegin(), settings.cend(), std::back_inserter(sizes));
    setColumnSizes(sizes);
    setSortColumn(group.readEntry(SORT_COLUMN, 0),
                  group.readEntry(SORT_DESCENDING, true) ? Qt::DescendingOrder : Qt::AscendingOrder);
}

void Page::init()
{

}

Page::~Page() {}

void Page::saveTo(KConfigGroup &group) const
{

    group.writeEntry(TITLE_ENTRY,         m_title);
    group.writeEntry(STRING_FILTER_ENTRY, stringFilter());
    group.writeEntry(KEY_FILTER_ENTRY,    keyFilter() ? keyFilter()->id() : QString());
    group.writeEntry(HIERARCHICAL_VIEW_ENTRY, isHierarchicalView());
    QList<int> settings;
    const auto sizes = columnSizes();
    settings.reserve(sizes.size());
    std::copy(sizes.cbegin(), sizes.cend(), std::back_inserter(settings));
    group.writeEntry(COLUMN_SIZES,        settings);
    group.writeEntry(SORT_COLUMN,         sortColumn());
    group.writeEntry(SORT_DESCENDING,     sortOrder() == Qt::DescendingOrder);
}

void Page::setStringFilter(const QString &filter)
{
    if (!m_canChangeStringFilter) {
        return;
    }
    KeyTreeView::setStringFilter(filter);
}

void Page::setKeyFilter(const std::shared_ptr<KeyFilter> &filter)
{
    if (!canChangeKeyFilter()) {
        return;
    }
    const QString oldTitle = title();
    KeyTreeView::setKeyFilter(filter);
    const QString newTitle = title();
    if (oldTitle != newTitle) {
        Q_EMIT titleChanged(newTitle);
    }
}

void Page::setTitle(const QString &t)
{
    if (t == m_title) {
        return;
    }
    if (!m_canBeRenamed) {
        return;
    }
    const QString oldTitle = title();
    m_title = t;
    const QString newTitle = title();
    if (oldTitle != newTitle) {
        Q_EMIT titleChanged(newTitle);
    }
}

#if 0 // not used
void Page::setToolTip(const QString &tip)
{
    if (tip == m_toolTip) {
        return;
    }
    if (!m_canBeRenamed) {
        return;
    }
    const QString oldTip = toolTip();
    m_toolTip = tip;
    const QString newTip = toolTip();
    if (oldTip != newTip) {
        Q_EMIT titleChanged(title());
    }
}
#endif

void Page::setHierarchicalView(bool on)
{
    if (!m_canChangeHierarchical) {
        return;
    }
    KeyTreeView::setHierarchicalView(on);
}

void Page::setTemporary(bool on)
{
    if (on == m_isTemporary) {
        return;
    }
    m_isTemporary = on;
    if (on) {
        setKeyFilter(std::shared_ptr<KeyFilter>());
    }
}

//
//
// TabWidget
//
//

class TabWidget::Private
{
    friend class ::Kleo::TabWidget;
    TabWidget *const q;
public:
    explicit Private(TabWidget *qq);
    ~Private() {}

private:
    void slotContextMenu(const QPoint &p);
    void currentIndexChanged(int index);
    void slotPageTitleChanged(const QString &title);
    void slotPageKeyFilterChanged(const std::shared_ptr<KeyFilter> &filter);
    void slotPageStringFilterChanged(const QString &filter);
    void slotPageHierarchyChanged(bool on);

#ifndef QT_NO_INPUTDIALOG
    void slotRenameCurrentTab()
    {
        renamePage(currentPage());
    }
#endif // QT_NO_INPUTDIALOG
    void slotNewTab();
    void slotDuplicateCurrentTab()
    {
        duplicatePage(currentPage());
    }
    void slotCloseCurrentTab()
    {
        closePage(currentPage());
    }
    void slotMoveCurrentTabLeft()
    {
        movePageLeft(currentPage());
    }
    void slotMoveCurrentTabRight()
    {
        movePageRight(currentPage());
    }
    void slotToggleHierarchicalView(bool on)
    {
        toggleHierarchicalView(currentPage(), on);
    }
    void slotExpandAll()
    {
        expandAll(currentPage());
    }
    void slotCollapseAll()
    {
        collapseAll(currentPage());
    }

#ifndef QT_NO_INPUTDIALOG
    void renamePage(Page *page);
#endif
    void duplicatePage(Page *page);
    void closePage(Page *page);
    void movePageLeft(Page *page);
    void movePageRight(Page *page);
    void toggleHierarchicalView(Page *page, bool on);
    void expandAll(Page *page);
    void collapseAll(Page *page);

    void enableDisableCurrentPageActions();
    void enableDisablePageActions(const std::vector<QAction *> &actions, const Page *page);

    Page *currentPage() const
    {
        Q_ASSERT(!tabWidget.currentWidget() || qobject_cast<Page *>(tabWidget.currentWidget()));
        return static_cast<Page *>(tabWidget.currentWidget());
    }
    Page *page(unsigned int idx) const
    {
        Q_ASSERT(!tabWidget.widget(idx) || qobject_cast<Page *>(tabWidget.widget(idx)));
        return static_cast<Page *>(tabWidget.widget(idx));
    }

    Page *senderPage() const
    {
        QObject *const sender = q->sender();
        Q_ASSERT(!sender || qobject_cast<Page *>(sender));
        return static_cast<Page *>(sender);
    }

    bool isSenderCurrentPage() const
    {
        Page *const sp = senderPage();
        return sp && sp == currentPage();
    }

    QTreeView *addView(Page *page, Page *columnReference);
    void setCornerAction(QAction *action, Qt::Corner corner);

private:
    AbstractKeyListModel *flatModel;
    AbstractKeyListModel *hierarchicalModel;
    QTabWidget tabWidget;
    QVBoxLayout layout;
    enum {
        Rename,
        Duplicate,
        Close,
        MoveLeft,
        MoveRight,
        Hierarchical,
        ExpandAll,
        CollapseAll,

        NumPageActions
    };
    QAction *newAction = nullptr;
    std::vector<QAction *> currentPageActions;
    std::vector<QAction *> otherPageActions;
    bool actionsCreated;
};

TabWidget::Private::Private(TabWidget *qq)
    : q(qq),
      flatModel(nullptr),
      hierarchicalModel(nullptr),
      tabWidget(q),
      layout(q),
      actionsCreated(false)
{
    KDAB_SET_OBJECT_NAME(tabWidget);
    KDAB_SET_OBJECT_NAME(layout);

    layout.setContentsMargins(0, 0, 0, 0);
    layout.addWidget(&tabWidget);

    tabWidget.tabBar()->hide();
    tabWidget.setMovable(true);

    tabWidget.tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(&tabWidget, SIGNAL(currentChanged(int)), q, SLOT(currentIndexChanged(int)));
    connect(tabWidget.tabBar(), &QWidget::customContextMenuRequested, q, [this](const QPoint & p) {
        slotContextMenu(p);
    });

}

void TabWidget::Private::slotContextMenu(const QPoint &p)
{
    const int tabUnderPos = tabWidget.tabBar()->tabAt(p);
    Page *const contextMenuPage = static_cast<Page *>(tabWidget.widget(tabUnderPos));
    const Page *const current = currentPage();

    const std::vector<QAction *> actions = contextMenuPage == current ? currentPageActions : otherPageActions;

    enableDisablePageActions(actions, contextMenuPage);

    QMenu menu;
    menu.addAction(actions[Rename]);
    menu.addSeparator();
    menu.addAction(newAction);
    menu.addAction(actions[Duplicate]);
    menu.addSeparator();
    menu.addAction(actions[MoveLeft]);
    menu.addAction(actions[MoveRight]);
    menu.addSeparator();
    menu.addAction(actions[Close]);

    const QAction *const action = menu.exec(tabWidget.tabBar()->mapToGlobal(p));

    if (contextMenuPage == current || action == newAction) {
        return;    // performed through signal/slot connections...
    }

#ifndef QT_NO_INPUTDIALOG
    if (action == otherPageActions[Rename]) {
        renamePage(contextMenuPage);
    }
#endif // QT_NO_INPUTDIALOG
    else if (action == otherPageActions[Duplicate]) {
        duplicatePage(contextMenuPage);
    } else if (action == otherPageActions[Close]) {
        closePage(contextMenuPage);
    } else if (action == otherPageActions[MoveLeft]) {
        movePageLeft(contextMenuPage);
    } else if (action == otherPageActions[MoveRight]) {
        movePageRight(contextMenuPage);
    }

}

void TabWidget::Private::currentIndexChanged(int index)
{
    const Page *const page = this->page(index);
    Q_EMIT q->currentViewChanged(page ? page->view() : nullptr);
    Q_EMIT q->keyFilterChanged(page ? page->keyFilter() : std::shared_ptr<KeyFilter>());
    Q_EMIT q->stringFilterChanged(page ? page->stringFilter() : QString());
    enableDisableCurrentPageActions();
}

void TabWidget::Private::enableDisableCurrentPageActions()
{
    const Page *const page = currentPage();

    Q_EMIT q->enableChangeStringFilter(page && page->canChangeStringFilter());
    Q_EMIT q->enableChangeKeyFilter(page && page->canChangeKeyFilter());

    enableDisablePageActions(currentPageActions, page);
}

void TabWidget::Private::enableDisablePageActions(const std::vector<QAction *> &actions, const Page *p)
{
    actions[Rename]      ->setEnabled(p && p->canBeRenamed());
    actions[Duplicate]   ->setEnabled(p);
    actions[Close]       ->setEnabled(p && p->canBeClosed() && tabWidget.count() > 1);
    actions[MoveLeft]    ->setEnabled(p && tabWidget.indexOf(const_cast<Page *>(p)) != 0);
    actions[MoveRight]   ->setEnabled(p && tabWidget.indexOf(const_cast<Page *>(p)) != tabWidget.count() - 1);
    actions[Hierarchical]->setEnabled(p && p->canChangeHierarchical());
    actions[Hierarchical]->setChecked(p && p->isHierarchicalView());
    actions[ExpandAll]   ->setEnabled(p && p->isHierarchicalView());
    actions[CollapseAll] ->setEnabled(p && p->isHierarchicalView());

    if (tabWidget.count() < 2) {
        tabWidget.tabBar()->hide();
    } else {
        tabWidget.tabBar()->show();
    }
}

void TabWidget::Private::slotPageTitleChanged(const QString &)
{
    if (Page *const page = senderPage()) {
        const int idx = tabWidget.indexOf(page);
        tabWidget.setTabText(idx, page->title());
        tabWidget.setTabToolTip(idx, page->toolTip());
    }
}

void TabWidget::Private::slotPageKeyFilterChanged(const std::shared_ptr<KeyFilter> &kf)
{
    if (isSenderCurrentPage()) {
        Q_EMIT q->keyFilterChanged(kf);
    }
}

void TabWidget::Private::slotPageStringFilterChanged(const QString &filter)
{
    if (isSenderCurrentPage()) {
        Q_EMIT q->stringFilterChanged(filter);
    }
}

void TabWidget::Private::slotPageHierarchyChanged(bool)
{
    enableDisableCurrentPageActions();
}

void TabWidget::Private::slotNewTab()
{
    const KConfigGroup group = KSharedConfig::openConfig()->group(QString::asprintf("View #%u", tabWidget.count()));
    Page *page = new Page(QString(), QStringLiteral("all-certificates"), QString(), nullptr, QString(), nullptr, group);
    addView(page, currentPage());
    tabWidget.setCurrentIndex(tabWidget.count() - 1);
}

void TabWidget::Private::renamePage(Page *page)
{
    if (!page) {
        return;
    }
    bool ok;
    const QString text = QInputDialog::getText(q, i18n("Rename Tab"), i18n("New tab title:"), QLineEdit::Normal, page->title(), &ok);
    if (!ok) {
        return;
    }
    page->setTitle(text);
}

void TabWidget::Private::duplicatePage(Page *page)
{
    if (!page) {
        return;
    }
    Page *const clone = page->clone();
    Q_ASSERT(clone);
    clone->liftAllRestrictions();
    addView(clone, page);
}

void TabWidget::Private::closePage(Page *page)
{
    if (!page || !page->canBeClosed() || tabWidget.count() <= 1) {
        return;
    }
    Q_EMIT q->viewAboutToBeRemoved(page->view());
    tabWidget.removeTab(tabWidget.indexOf(page));
    enableDisableCurrentPageActions();
}

void TabWidget::Private::movePageLeft(Page *page)
{
    if (!page) {
        return;
    }
    const int idx = tabWidget.indexOf(page);
    if (idx <= 0) {
        return;
    }
    tabWidget.tabBar()->moveTab(idx, idx - 1);
    enableDisableCurrentPageActions();
}

void TabWidget::Private::movePageRight(Page *page)
{
    if (!page) {
        return;
    }
    const int idx = tabWidget.indexOf(page);
    if (idx < 0 || idx >= tabWidget.count() - 1) {
        return;
    }
    tabWidget.tabBar()->moveTab(idx, idx + 1);
    enableDisableCurrentPageActions();
}

void TabWidget::Private::toggleHierarchicalView(Page *page, bool on)
{
    if (!page) {
        return;
    }
    page->setHierarchicalView(on);
}

void TabWidget::Private::expandAll(Page *page)
{
    if (!page || !page->view()) {
        return;
    }
    page->view()->expandAll();
}

void TabWidget::Private::collapseAll(Page *page)
{
    if (!page || !page->view()) {
        return;
    }
    page->view()->collapseAll();
}

TabWidget::TabWidget(QWidget *p, Qt::WindowFlags f)
    : QWidget(p, f), d(new Private(this))
{

}

TabWidget::~TabWidget() {
    saveViews(KSharedConfig::openConfig().data());
}

void TabWidget::setFlatModel(AbstractKeyListModel *model)
{
    if (model == d->flatModel) {
        return;
    }
    d->flatModel = model;
    for (unsigned int i = 0, end = count(); i != end; ++i)
        if (Page *const page = d->page(i)) {
            page->setFlatModel(model);
        }
}

AbstractKeyListModel *TabWidget::flatModel() const
{
    return d->flatModel;
}

void TabWidget::setHierarchicalModel(AbstractKeyListModel *model)
{
    if (model == d->hierarchicalModel) {
        return;
    }
    d->hierarchicalModel = model;
    for (unsigned int i = 0, end = count(); i != end; ++i)
        if (Page *const page = d->page(i)) {
            page->setHierarchicalModel(model);
        }
}

AbstractKeyListModel *TabWidget::hierarchicalModel() const
{
    return d->hierarchicalModel;
}

void TabWidget::Private::setCornerAction(QAction *action, Qt::Corner corner)
{
    if (!action) {
        return;
    }
    auto b = new QToolButton;
    b->setDefaultAction(action);
    tabWidget.setCornerWidget(b, corner);
}

void TabWidget::setStringFilter(const QString &filter)
{
    if (Page *const page = d->currentPage()) {
        page->setStringFilter(filter);
    }
}

void TabWidget::setKeyFilter(const std::shared_ptr<KeyFilter> &filter)
{
    if (!filter) {
        qCDebug(KLEOPATRA_LOG) << "TabWidget::setKeyFilter() trial to set filter=NULL";
        return;
    }

    if (Page *const page = d->currentPage()) {
        page->setKeyFilter(filter);
    }
}

std::vector<QAbstractItemView *> TabWidget::views() const
{
    std::vector<QAbstractItemView *> result;
    const unsigned int N = count();
    result.reserve(N);
    for (unsigned int i = 0; i != N; ++i)
        if (const Page *const p = d->page(i)) {
            result.push_back(p->view());
        }
    return result;
}

QAbstractItemView *TabWidget::currentView() const
{
    if (Page *const page = d->currentPage()) {
        return page->view();
    } else {
        return nullptr;
    }
}

KeyListModelInterface *TabWidget::currentModel() const
{
    const QAbstractItemView *const view = currentView();
    if (!view) {
        return nullptr;
    }
    auto const proxy = qobject_cast<QAbstractProxyModel *>(view->model());
    if (!proxy) {
        return nullptr;
    }
    return dynamic_cast<KeyListModelInterface *>(proxy);
}

unsigned int TabWidget::count() const
{
    return d->tabWidget.count();
}

void TabWidget::setMultiSelection(bool on)
{
    for (unsigned int i = 0, end = count(); i != end; ++i)
        if (const Page *const p = d->page(i))
            if (QTreeView *const view = p->view()) {
                view->setSelectionMode(on ? QAbstractItemView::ExtendedSelection : QAbstractItemView::SingleSelection);
            }
}

void TabWidget::createActions(KActionCollection *coll)
{
    if (!coll) {
        return;
    }
    const action_data actionDataNew = {
        "window_new_tab", i18n("New Tab"), i18n("Open a new tab"),
        "tab-new-background", this, SLOT(slotNewTab()), QStringLiteral("CTRL+SHIFT+N"), false, true
    };

    d->newAction = make_action_from_data(actionDataNew, coll);

    struct action_data actionData[] = {
        {
            "window_rename_tab", i18n("Rename Tab..."), i18n("Rename this tab"),
            "edit-rename", this, SLOT(slotRenameCurrentTab()), QStringLiteral("CTRL+SHIFT+R"), false, false
        },
        {
            "window_duplicate_tab", i18n("Duplicate Tab"), i18n("Duplicate this tab"),
            "tab-duplicate", this, SLOT(slotDuplicateCurrentTab()), QStringLiteral("CTRL+SHIFT+D"), false, true
        },
        {
            "window_close_tab", i18n("Close Tab"), i18n("Close this tab"),
            "tab-close", this, SLOT(slotCloseCurrentTab()), QStringLiteral("CTRL+SHIFT+W"), false, false
        }, // ### CTRL-W when available
        {
            "window_move_tab_left", i18n("Move Tab Left"), i18n("Move this tab left"),
            nullptr, this, SLOT(slotMoveCurrentTabLeft()), QStringLiteral("CTRL+SHIFT+LEFT"), false, false
        },
        {
            "window_move_tab_right", i18n("Move Tab Right"), i18n("Move this tab right"),
            nullptr, this, SLOT(slotMoveCurrentTabRight()), QStringLiteral("CTRL+SHIFT+RIGHT"), false, false
        },
        {
            "window_view_hierarchical", i18n("Hierarchical Certificate List"), QString(),
            nullptr, this, SLOT(slotToggleHierarchicalView(bool)), QString(), true, false
        },
        {
            "window_expand_all", i18n("Expand All"), QString(),
            nullptr, this, SLOT(slotExpandAll()), QStringLiteral("CTRL+."), false, false
        },
        {
            "window_collapse_all", i18n("Collapse All"), QString(),
            nullptr, this, SLOT(slotCollapseAll()), QStringLiteral("CTRL+,"), false, false
        },
    };

    d->currentPageActions.reserve(d->NumPageActions);
    for (int i = 0; i < d->NumPageActions; ++i) {
        d->currentPageActions.push_back(make_action_from_data(actionData[i], coll));
    }

    d->otherPageActions.reserve(d->NumPageActions);
    for (int i = 0; i < d->NumPageActions; ++i) {
        // create actions for the context menu of the currently not active tabs,
        // but do not add those actions to the action collection
        const action_data ad = actionData[i];
        auto action = new QAction(ad.text, coll);
        if (ad.icon) {
            action->setIcon(QIcon::fromTheme(QLatin1String(ad.icon)));
        }
        action->setEnabled(ad.enabled);
        d->otherPageActions.push_back(action);
    }

    d->setCornerAction(d->newAction,                 Qt::TopLeftCorner);
    d->setCornerAction(d->currentPageActions[d->Close], Qt::TopRightCorner);
    d->actionsCreated = true;
}

QAbstractItemView *TabWidget::addView(const QString &title, const QString &id, const QString &text)
{
    const KConfigGroup group = KSharedConfig::openConfig()->group(QString::asprintf("View #%u", d->tabWidget.count()));
    Page *page = new Page(title, id, text, nullptr, QString(), nullptr, group);
    return d->addView(page, d->currentPage());
}

QAbstractItemView *TabWidget::addView(const KConfigGroup &group)
{
    return d->addView(new Page(group), nullptr);
}

QAbstractItemView *TabWidget::addTemporaryView(const QString &title, AbstractKeyListSortFilterProxyModel *proxy, const QString &tabToolTip)
{
    const KConfigGroup group = KSharedConfig::openConfig()->group("KeyTreeView_default");
    Page *const page = new Page(title, QString(), QString(), proxy, tabToolTip, nullptr, group);
    page->setTemporary(true);
    QAbstractItemView *v = d->addView(page, d->currentPage());
    d->tabWidget.setCurrentIndex(d->tabWidget.count() - 1);
    return v;
}

QTreeView *TabWidget::Private::addView(Page *page, Page *columnReference)
{
    if (!page) {
        return nullptr;
    }

    if (!actionsCreated) {
        auto coll = new KActionCollection(q);
        q->createActions(coll);
    }

    page->setFlatModel(flatModel);
    page->setHierarchicalModel(hierarchicalModel);

    connect(page, SIGNAL(titleChanged(QString)), q, SLOT(slotPageTitleChanged(QString)));
    connect(page, SIGNAL(keyFilterChanged(std::shared_ptr<Kleo::KeyFilter>)), q, SLOT(slotPageKeyFilterChanged(std::shared_ptr<Kleo::KeyFilter>)));
    connect(page, SIGNAL(stringFilterChanged(QString)), q, SLOT(slotPageStringFilterChanged(QString)));
    connect(page, SIGNAL(hierarchicalChanged(bool)), q, SLOT(slotPageHierarchyChanged(bool)));

    if (columnReference) {
        page->setColumnSizes(columnReference->columnSizes());
        page->setSortColumn(columnReference->sortColumn(), columnReference->sortOrder());
    }

    QAbstractItemView *const previous = q->currentView();
    const int tabIndex = tabWidget.addTab(page, page->title());
    tabWidget.setTabToolTip(tabIndex, page->toolTip());
    // work around a bug in QTabWidget (tested with 4.3.2) not emitting currentChanged() when the first widget is inserted
    QAbstractItemView *const current = q->currentView();
    if (previous != current) {
        currentIndexChanged(tabWidget.currentIndex());
    }
    enableDisableCurrentPageActions();
    QTreeView *view = page->view();
    Q_EMIT q->viewAdded(view);
    return view;
}

static QStringList extractViewGroups(const KConfig *config)
{
    return config ? config->groupList().filter(QRegularExpression(QStringLiteral("^View #\\d+$"))) : QStringList();
}

// work around deleteGroup() not deleting groups out of groupList():
static const bool KCONFIG_DELETEGROUP_BROKEN = true;

void TabWidget::loadViews(const KConfig *config)
{
    if (config) {
        QStringList groupList = extractViewGroups(config);
        groupList.sort();
        for (const QString &group : std::as_const(groupList)) {
            const KConfigGroup kcg(config, group);
            if (!KCONFIG_DELETEGROUP_BROKEN || kcg.readEntry("magic", 0U) == 0xFA1AFE1U) {
                addView(kcg);
            }
        }
    }
    if (!count()) {
        // add default view:
        addView(QString(), QStringLiteral("all-certificates"));
    }
}

void TabWidget::saveViews(KConfig *config) const
{
    if (!config) {
        return;
    }
    const auto extraView{extractViewGroups(config)};
    for (const QString &group : extraView) {
        config->deleteGroup(group);
    }
    unsigned int vg = 0;
    for (unsigned int i = 0, end = count(); i != end; ++i) {
        if (const Page *const p = d->page(i)) {
            if (p->isTemporary()) {
                continue;
            }
            KConfigGroup group(config, QString::asprintf("View #%u", vg++));
            p->saveTo(group);
            if (KCONFIG_DELETEGROUP_BROKEN) {
                group.writeEntry("magic", 0xFA1AFE1U);
            }
        }
    }
}

static void xconnect(const QObject *o1, const char *signal, const QObject *o2, const char *slot)
{
    QObject::connect(o1, signal, o2, slot);
    QObject::connect(o2, signal, o1, slot);
}

void TabWidget::connectSearchBar(QObject *sb)
{
    xconnect(sb, SIGNAL(stringFilterChanged(QString)),
             this, SLOT(setStringFilter(QString)));
    xconnect(sb, SIGNAL(keyFilterChanged(std::shared_ptr<Kleo::KeyFilter>)),
             this, SLOT(setKeyFilter(std::shared_ptr<Kleo::KeyFilter>)));
    connect(this, SIGNAL(enableChangeStringFilter(bool)),
            sb, SLOT(setChangeStringFilterEnabled(bool)));
    connect(this, SIGNAL(enableChangeKeyFilter(bool)),
            sb, SLOT(setChangeKeyFilterEnabled(bool)));
}

#include "moc_tabwidget.cpp"
#include "tabwidget.moc"
