#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <functional>

#include "accounts.h"
#include "backend.h"

// Makes the Backend of one account; main and the harness give each new one
// the old drive-sync check, tests give fakes.
using BackendFactory = std::function<Backend *(const BackendPaths &paths)>;

// The window's accounts (spec 12, "What the user sees"): one Backend per
// account the resolver lists, or one over the view with no account, and
// which one the window shows. QML reads the shown one as the "backend"
// context property, which main moves to current() on selectedChanged.
//
// It follows the resolver as 5.1's single Backend did. Each Backend asks the
// whole set through its own resolver (setResolver), gets its own entry back,
// and queues one reconcile, which makes a Backend for an account that
// appeared and drops the one of an account that went. With one Backend and
// one entry the Backend adopts that entry in place, whatever the ids (a first
// setup, a takeover, a cancelled setup, the move to account1), so the window
// keeps the same object.
class AccountList : public QObject {
    Q_OBJECT
    // {id, label, folder, tone, status} per account, in the resolver's order.
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)
    Q_PROPERTY(int count READ count NOTIFY accountsChanged)
    Q_PROPERTY(bool several READ several NOTIFY accountsChanged)
    Q_PROPERTY(int selected READ selected NOTIFY selectedChanged)
    // Its own signal: a label can change (another account's folder got the
    // same name) while the selection stays.
    Q_PROPERTY(QString currentLabel READ currentLabel NOTIFY currentLabelChanged)
    Q_PROPERTY(QObject *current READ current NOTIFY selectedChanged)

public:
    AccountList(std::function<WindowAccounts()> resolve, BackendFactory make, QObject *parent = nullptr);
    ~AccountList() override;

    QVariantList accounts() const { return m_accounts; }
    int count() const { return int(m_backends.size()); }
    bool several() const { return m_backends.size() > 1; }
    int selected() const { return int(m_backends.indexOf(m_current)); }
    QString currentLabel() const { return m_labels.value(m_current); }
    Backend *current() const { return m_current; }
    QList<Backend *> backends() const { return m_backends; }

    Q_INVOKABLE void select(int index);
    Q_INVOKABLE void selectNext();      // wraps
    Q_INVOKABLE void selectPrevious();  // wraps
    // False, and the selection unchanged, for an id not listed.
    bool selectId(const QString &id);
    // Resolves now and reconciles.
    Q_INVOKABLE void reload();

signals:
    void accountsChanged();
    void selectedChanged();
    void currentLabelChanged();

private:
    WindowAccount resolveFor(const Backend *backend);
    void reconcile();
    void rebuildAccounts();
    void setCurrent(Backend *backend);

    std::function<WindowAccounts()> m_resolve;
    BackendFactory m_make;
    QList<Backend *> m_backends;
    QHash<const Backend *, QString> m_labels;
    Backend *m_current = nullptr;
    QVariantList m_accounts;
    // A Backend's resolver queues one reconcile, however many of them ask.
    QTimer m_reconcileSoon;
    // Halfway through a live move the resolver can see no account at all:
    // the list keeps what it holds and asks again each second.
    QTimer m_moveWait;
    // While a reconcile runs, the Backends it asks to follow their entry read
    // the set it resolved instead of resolving again and queueing another.
    bool m_reconciling = false;
    WindowAccounts m_set;
};
