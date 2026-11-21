#include "accountlist.h"

#include <QVariantMap>
#include <algorithm>

namespace {

// The same account as Backend::reloadConfig tells it: another id, config,
// state folder or unit is another account to adopt.
bool sameAccount(const BackendPaths &a, const BackendPaths &b) {
    return a.id == b.id && a.config == b.config && a.appState == b.appState && accountUnits(a) == accountUnits(b);
}

bool hasNotice(const QList<AccountNotice> &notices, const QString &kind) {
    return std::any_of(notices.cbegin(), notices.cend(), [&](const AccountNotice &n) { return n.kind == kind; });
}

}  // namespace

AccountList::AccountList(std::function<WindowAccounts()> resolve, BackendFactory make, QObject *parent)
    : QObject(parent), m_resolve(std::move(resolve)), m_make(std::move(make)) {
    m_reconcileSoon.setSingleShot(true);
    m_reconcileSoon.setInterval(0);
    connect(&m_reconcileSoon, &QTimer::timeout, this, &AccountList::reconcile);
    m_moveWait.setSingleShot(true);
    m_moveWait.setInterval(1000);
    connect(&m_moveWait, &QTimer::timeout, this, &AccountList::reconcile);
    // The window opens on the first account: current() is never null.
    reconcile();
}

AccountList::~AccountList() {
    // Gone while the list is still whole: a Backend's resolver and its
    // changed signal reach this object.
    for (Backend *backend : std::as_const(m_backends)) {
        disconnect(backend, nullptr, this, nullptr);
        delete backend;
    }
}

void AccountList::reload() { reconcile(); }

WindowAccount AccountList::resolveFor(const Backend *backend) {
    // Never makes or deletes a Backend here: this runs inside that Backend's
    // reloadConfig. The reconcile that does it runs from the event loop.
    const WindowAccounts set = m_reconciling ? m_set : m_resolve();
    if (!m_reconciling)
        m_reconcileSoon.start();
    if (m_backends.size() == 1 && m_backends.first() == backend && set.accounts.size() == 1)
        return {set.accounts.first(), set.notices};
    for (const BackendPaths &account : set.accounts)
        if (account.id == backend->paths().id)
            return {account, set.notices};
    // Its account is gone: it keeps what it shows until the reconcile drops it.
    return {backend->paths(), set.notices};
}

void AccountList::reconcile() {
    m_reconcileSoon.stop();
    const WindowAccounts set = m_resolve();
    // A move under way (its journal, and no notice that it stopped) can be
    // caught between two renames, with no account listed: keep what is held
    // until it ends. The first reconcile makes the window's Backend anyway.
    if (!m_backends.isEmpty() && moveInProgress(set.accounts.first().globalState())
        && !hasNotice(set.notices, "moveStuck")) {
        m_moveWait.start();
        return;
    }
    Backend *const oldCurrent = m_current;
    const int oldIndex = selected();
    const QString oldLabel = currentLabel();

    m_set = set;
    m_reconciling = true;
    // One Backend and one entry: the same object follows it, whatever the
    // ids. Otherwise each account keeps the Backend of its id.
    const bool inPlace = m_backends.size() == 1 && set.accounts.size() == 1;
    QList<Backend *> kept;
    QHash<const Backend *, QString> labels;
    for (const BackendPaths &account : set.accounts) {
        Backend *backend = inPlace ? m_backends.first() : nullptr;
        for (qsizetype i = 0; !backend && i < m_backends.size(); ++i)
            if (m_backends.at(i)->paths().id == account.id && !kept.contains(m_backends.at(i)))
                backend = m_backends.at(i);
        if (backend) {
            // Its resolver hands it this entry, read from m_set.
            if (!sameAccount(backend->paths(), account))
                backend->reloadConfig();
        } else {
            backend = m_make(account);
            backend->setParent(this);
            m_backends << backend;
            connect(backend, &Backend::changed, this, &AccountList::rebuildAccounts);
            backend->setResolver([this, backend] { return resolveFor(backend); });
        }
        kept << backend;
        labels.insert(backend, account.label);
    }
    QList<Backend *> removed;
    for (Backend *backend : std::as_const(m_backends))
        if (!kept.contains(backend))
            removed << backend;
    m_backends = kept;
    m_labels = labels;
    m_reconciling = false;
    m_set = {};

    if (!m_current || !m_backends.contains(m_current))
        m_current = m_backends.first();
    for (Backend *backend : std::as_const(removed))
        disconnect(backend, nullptr, this, nullptr);
    rebuildAccounts();
    // The window moves "backend" to the new one here, before the old one
    // goes, so QML never reads a deleted Backend.
    if (oldCurrent && (m_current != oldCurrent || selected() != oldIndex))
        emit selectedChanged();
    if (oldCurrent && currentLabel() != oldLabel)
        emit currentLabelChanged();
    for (Backend *backend : std::as_const(removed))
        backend->deleteLater();
}

void AccountList::rebuildAccounts() {
    QVariantList accounts;
    for (const Backend *backend : std::as_const(m_backends))
        accounts.append(QVariantMap{{"id", backend->accountId()},
                                    {"label", m_labels.value(backend)},
                                    {"folder", backend->drivePath()},
                                    {"tone", backend->tone()},
                                    {"status", backend->status()}});
    // Backend::changed comes with every recompute: only a real change is said.
    if (accounts == m_accounts)
        return;
    m_accounts = accounts;
    emit accountsChanged();
}

void AccountList::setCurrent(Backend *backend) {
    if (!backend || backend == m_current)
        return;
    const QString oldLabel = currentLabel();
    m_current = backend;
    emit selectedChanged();
    if (currentLabel() != oldLabel)
        emit currentLabelChanged();
}

void AccountList::select(int index) {
    if (index < 0 || index >= m_backends.size())
        return;
    setCurrent(m_backends.at(index));
}

void AccountList::selectNext() {
    if (m_backends.size() > 1)
        select((selected() + 1) % int(m_backends.size()));
}

void AccountList::selectPrevious() {
    if (m_backends.size() > 1)
        select((selected() + int(m_backends.size()) - 1) % int(m_backends.size()));
}

bool AccountList::selectId(const QString &id) {
    for (Backend *backend : std::as_const(m_backends))
        if (backend->accountId() == id) {
            setCurrent(backend);
            return true;
        }
    return false;
}
