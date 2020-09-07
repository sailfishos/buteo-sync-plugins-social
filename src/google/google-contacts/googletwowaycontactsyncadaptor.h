/****************************************************************************
 **
 ** Copyright (c) 2014 - 2019 Jolla Ltd.
 ** Copyright (c) 2020 Open Mobile Platform LLC.
 **
 ** This program/library is free software; you can redistribute it and/or
 ** modify it under the terms of the GNU Lesser General Public License
 ** version 2.1 as published by the Free Software Foundation.
 **
 ** This program/library is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 ** Lesser General Public License for more details.
 **
 ** You should have received a copy of the GNU Lesser General Public
 ** License along with this program/library; if not, write to the Free
 ** Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 ** 02110-1301 USA
 **
 ****************************************************************************/

#ifndef GOOGLETWOWAYCONTACTSYNCADAPTOR_H
#define GOOGLETWOWAYCONTACTSYNCADAPTOR_H

#include "googledatatypesyncadaptor.h"
#include "googlecontactstream.h"

#include <twowaycontactsyncadaptor.h>

#include <QContactManager>
#include <QContact>
#include <QDateTime>
#include <QList>
#include <QPair>

QTCONTACTS_USE_NAMESPACE

class GoogleContactImageDownloader;
class GoogleTwoWayContactSyncAdaptor;

class GoogleContactSqliteSyncAdaptor : public QObject, public QtContactsSqliteExtensions::TwoWayContactSyncAdaptor
{
    Q_OBJECT
public:
    GoogleContactSqliteSyncAdaptor(int accountId, GoogleTwoWayContactSyncAdaptor *parent);
   ~GoogleContactSqliteSyncAdaptor();

    int accountId() const;

    virtual bool determineRemoteCollections() override;
    virtual bool deleteRemoteCollection(const QContactCollection &collection) override;
    virtual bool determineRemoteContacts(const QContactCollection &collection) override;

    virtual bool determineRemoteContactChanges(const QContactCollection &collection,
                                               const QList<QContact> &localAddedContacts,
                                               const QList<QContact> &localModifiedContacts,
                                               const QList<QContact> &localDeletedContacts,
                                               const QList<QContact> &localUnmodifiedContacts,
                                               QContactManager::Error *error) override;

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

    QContactCollection m_collection;

private:
    GoogleTwoWayContactSyncAdaptor *q;
    int m_accountId = 0;
};

class GoogleTwoWayContactSyncAdaptor : public GoogleDataTypeSyncAdaptor
{
    Q_OBJECT
public:
    enum DataRequestType {
        ContactRequest,
        ContactGroupRequest
    };

    enum ContactChangeNotifier {
        NoContactChangeNotifier,
        DetermineRemoteContacts,
        DetermineRemoteContactChanges
    };
    Q_ENUM(ContactChangeNotifier)

    GoogleTwoWayContactSyncAdaptor(QObject *parent);
   ~GoogleTwoWayContactSyncAdaptor();

    virtual QString syncServiceName() const override;
    virtual void sync(const QString &dataTypeString, int accountId) override;

    void requestData(int accountId,
                     int startIndex,
                     const QString &continuationRequest,
                     const QDateTime &syncTimestamp,
                     DataRequestType requestType,
                     ContactChangeNotifier contactChangeNotifier = NoContactChangeNotifier);
    void upsyncLocalChanges(const QDateTime &localSince,
                            const QList<QContact> &locallyAdded,
                            const QList<QContact> &locallyModified,
                            const QList<QContact> &locallyDeleted,
                            int accountId);

    void storeSyncStateData(QContactCollection *collection);

    QContactManager *m_contactManager = nullptr;

protected:
    // implementing GoogleDataTypeSyncAdaptor interface
    void purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode mode);
    void beginSync(int accountId, const QString &accessToken);
    void finalize(int accountId);
    void finalCleanup();

private:
    class BatchedUpdate
    {
    public:
        QMultiMap<GoogleContactStream::UpdateType, QPair<QContact, QStringList> > batch;
        int batchCount = 0;
    };

    void determineRemoteChanges(const QDateTime &remoteSince, int accountId);
    void groupsFinishedHandler();
    void contactsFinishedHandler();
    void continueSync(int accountId,
                      const QString &accessToken,
                      GoogleTwoWayContactSyncAdaptor::ContactChangeNotifier contactChangeNotifier);
    void upsyncLocalChangesList(int accountId);
    bool batchRemoteChanges(int accountId, BatchedUpdate *batchedUpdate,
                            QList<QContact> *contacts, GoogleContactStream::UpdateType updateType);
    void storeToRemote(int accountId,
                       const QString &accessToken,
                       const QByteArray &encodedContactUpdates);
    void queueOutstandingAvatars(int accountId, const QString &accessToken);
    bool queueAvatarForDownload(int accountId, const QString &accessToken, const QString &contactGuid, const QString &imageUrl);
    void transformContactAvatars(QList<QContact> &remoteContacts, int accountId, const QString &accessToken);
    void downloadContactAvatarImage(int accountId, const QString &accessToken, const QUrl &imageUrl, const QString &filename);
    void imageDownloaded(const QString &url, const QString &path, const QVariantMap &metadata);

    void readSyncStateData(const QContactCollection &collection);

    void purgeAccount(int pid);
    void postFinishedHandler();
    void postErrorHandler();

    GoogleContactImageDownloader *m_workerObject = nullptr;

    QMap<int, GoogleContactSqliteSyncAdaptor *> m_sqliteSync;
    QMap<int, QString> m_accessTokens;
    QMap<int, QString> m_emailAddresses;

    QMap<int, QList<QContact> > m_remoteAdds;
    QMap<int, QList<QContact> > m_remoteMods;
    QMap<int, QList<QContact> > m_remoteDels;
    QMap<int, QList<QContact> > m_localAdds;
    QMap<int, QList<QContact> > m_localMods;
    QMap<int, QList<QContact> > m_localDels;

    QMap<int, QMap<QString, QStringList> > m_unsupportedXmlElements; // contact guid -> elements
    QMap<int, QMap<QString, QString> > m_contactEtags; // contact guid -> contact etag
    QMap<int, QMap<QString, QString> > m_contactIds; // contact guid -> contact id
    QMap<int, QMap<QString, QString> > m_contactAvatars; // contact guid -> remote avatar path

    QMap<int, int> m_apiRequestsRemaining;
    QMap<int, QMap<QString, QString> > m_queuedAvatarsForDownload; // contact guid -> remote avatar path
    QMap<int, QMap<QString, QString> > m_downloadedContactAvatars; // contact guid -> local file path

    bool m_allowFinalCleanup = false;
};

#endif // GOOGLETWOWAYCONTACTSYNCADAPTOR_H
