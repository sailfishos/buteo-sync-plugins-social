/****************************************************************************
 **
 ** Copyright (c) 2014 - 2019 Jolla Ltd.
 ** Copyright (c) 2020 Open Mobile Platform LLC.
 **
 ****************************************************************************/

#ifndef VKCONTACTSYNCADAPTOR_H
#define VKCONTACTSYNCADAPTOR_H

#include "vkdatatypesyncadaptor.h"

#include <twowaycontactsyncadaptor.h>

#include <QContactManager>
#include <QContact>
#include <QContactCollection>

#include <QList>

QTCONTACTS_USE_NAMESPACE

class VKContactImageDownloader;
class VKContactSyncAdaptor;

class VKContactSqliteSyncAdaptor : public QObject, public QtContactsSqliteExtensions::TwoWayContactSyncAdaptor
{
    Q_OBJECT
public:
    VKContactSqliteSyncAdaptor(int accountId, VKContactSyncAdaptor *parent);
   ~VKContactSqliteSyncAdaptor();

    virtual bool determineRemoteCollections() override;
    virtual bool deleteRemoteCollection(const QContactCollection &collection) override;
    virtual bool determineRemoteContacts(const QContactCollection &collection) override;
    virtual bool storeLocalChangesRemotely(const QContactCollection &collection,
                                           const QList<QContact> &addedContacts,
                                           const QList<QContact> &modifiedContacts,
                                           const QList<QContact> &deletedContacts) override;
    virtual void storeRemoteChangesLocally(const QContactCollection &collection,
                                           const QList<QContact> &addedContacts,
                                           const QList<QContact> &modifiedContacts,
                                           const QList<QContact> &deletedContacts) override;
    virtual void syncFinishedSuccessfully() override;
    virtual void syncFinishedWithError() override;

    static int accountIdForCollection(const QContactCollection &collection);

    QContactCollection m_collection;

private:
    VKContactSyncAdaptor *q;
    int m_accountId = 0;
};

class VKContactSyncAdaptor : public VKDataTypeSyncAdaptor
{
    Q_OBJECT
public:
    VKContactSyncAdaptor(QObject *parent);
   ~VKContactSyncAdaptor();

    virtual QString syncServiceName() const override;
    virtual void sync(const QString &dataTypeString, int accountId = 0) override;

    void requestData(int accountId, int startIndex = 0);
    void deleteDownloadedAvatar(const QContact &contact);

    QContactManager *m_contactManager = nullptr;

protected:
    // implementing VKDataTypeSyncAdaptor interface
    virtual void purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode mode) override;
    virtual void beginSync(int accountId, const QString &accessToken) override;
    virtual void finalize(int accountId) override;
    virtual void finalCleanup() override;
    virtual void retryThrottledRequest(const QString &request, const QVariantList &args, bool retryLimitReached) override;

private:
    void contactsFinishedHandler();
    QList<QContact> parseContacts(const QJsonArray &json, int accountId, const QString &accessToken);
    void transformContactAvatars(QList<QContact> &remoteContacts, int accountId, const QString &accessToken);
    bool queueAvatarForDownload(int accountId, const QString &accessToken, const QString &contactGuid, const QString &imageUrl);
    void imageDownloaded(const QString &url, const QString &path, const QVariantMap &metadata);

    VKContactImageDownloader *m_workerObject = nullptr;

    QMap<int, VKContactSqliteSyncAdaptor *> m_sqliteSync;
    QMap<int, QString> m_accessTokens;
    QMap<int, QList<QContact> > m_remoteContacts;

    QMap<int, int> m_apiRequestsRemaining;
    QMap<int, QMap<QString, QString> > m_queuedAvatarsForDownload; // contact guid -> remote avatar path
    QMap<int, QMap<QString, QString> > m_downloadedContactAvatars; // contact guid -> local file path
};

#endif // VKCONTACTSYNCADAPTOR_H
