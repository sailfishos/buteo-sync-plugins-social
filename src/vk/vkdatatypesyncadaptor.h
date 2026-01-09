/****************************************************************************
 **
 ** Copyright (C) 2013 Jolla Ltd.
 ** Contact: Chris Adams <chris.adams@jollamobile.com>
 **
 ****************************************************************************/

#ifndef VKDATATYPESYNCADAPTOR_H
#define VKDATATYPESYNCADAPTOR_H

#include "socialnetworksyncadaptor.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariantList>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslError>
#include <QtCore/QJsonObject>

namespace Accounts {
    class Account;
}
namespace SignOn {
    class Error;
    class SessionData;
}

class QJsonObject;

/*
    Abstract interface for all of the data-specific sync adaptors
    which pull data from the VK social network.
*/

class VKDataTypeSyncAdaptor : public SocialNetworkSyncAdaptor
{
    Q_OBJECT

public:
    class UserProfile
    {
    public:
        UserProfile();
        ~UserProfile();

        UserProfile(const UserProfile &other);
        UserProfile &operator=(const UserProfile &other);

        static UserProfile fromJsonObject(const QJsonObject &object);

        QString name() const;

        int uid;
        QString firstName;
        QString lastName;
        QString icon;
    };

    class GroupProfile
    {
    public:
        GroupProfile();
        ~GroupProfile();

        GroupProfile(const GroupProfile &other);
        GroupProfile &operator=(const GroupProfile &other);

        static GroupProfile fromJsonObject(const QJsonObject &object);

        int uid;
        QString name;
        QString screenName;
        QString icon;
    };

    VKDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::DataType dataType, QObject *parent);
    virtual ~VKDataTypeSyncAdaptor();

    void sync(const QString &dataTypeString, int accountId) override;

protected:
    QString clientId();
    virtual void updateDataForAccount(int accountId);
    virtual void beginSync(int accountId, const QString &accessToken) = 0;

    static QDateTime parseVKDateTime(const QJsonValue &v);
    static UserProfile findUserProfile(const QList<UserProfile> &profiles, int uid);
    static GroupProfile findGroupProfile(const QList<GroupProfile> &profiles, int uid);

    void enqueueThrottledRequest(const QString &request, const QVariantList &args, int interval = 0);
    bool enqueueServerThrottledRequestIfRequired(const QJsonObject &parsed,
                                                 const QString &request,
                                                 const QVariantList &args);
    virtual void retryThrottledRequest(const QString &request, const QVariantList &args, bool retryLimitReached) = 0;

protected Q_SLOTS:
    virtual void errorHandler(QNetworkReply::NetworkError err);
    virtual void sslErrorsHandler(const QList<QSslError> &errs);

private Q_SLOTS:
    void signOnError(const SignOn::Error &error);
    void signOnResponse(const SignOn::SessionData &responseData);
    void throttleTimerTimeout();

private:
    void loadClientId();
    void setCredentialsNeedUpdate(Accounts::Account *account);
    void signIn(Accounts::Account *account);
    bool m_triedLoading; // Is true if we tried to load (even if we failed)
    QString m_clientId;
    QTimer m_throttleTimer;
    QList<QPair<QString, QVariantList> > m_throttledRequestQueue;
};

#endif // VKDATATYPESYNCADAPTOR_H
