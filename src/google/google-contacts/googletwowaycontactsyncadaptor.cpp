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

#include "googletwowaycontactsyncadaptor.h"
#include "googlecontactimagedownloader.h"

#include "constants_p.h"
#include "trace.h"

#include <twowaycontactsyncadaptor_impl.h>
#include <qtcontacts-extensions_manager_impl.h>
#include <qcontactstatusflags_impl.h>
#include <contactmanagerengine.h>

#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>

#include <QtContacts/QContactCollectionFilter>
#include <QtContacts/QContact>

#include <Accounts/Manager>
#include <Accounts/Account>

static const char *IMAGE_DOWNLOADER_TOKEN_KEY = "url";
static const char *IMAGE_DOWNLOADER_IDENTIFIER_KEY = "identifier";

namespace {

const QString CollectionKeySyncToken = QStringLiteral("syncToken");
const QString CollectionKeySyncTokenDate = QStringLiteral("syncTokenDate");

QContactCollection findCollection(const QContactManager &contactManager, int accountId)
{
    const QList<QContactCollection> collections = contactManager.collections();
    for (const QContactCollection &collection : collections) {
        if (GooglePeople::ContactGroup::isMyContactsCollection(collection, accountId)) {
            return collection;
        }
    }
    return QContactCollection();
}

int indexOfContact(const QList<QContact> &contacts, const QContactId &contactId)
{
    for (int i = 0; i < contacts.count(); ++i) {
        if (contacts.at(i).id() == contactId) {
            return i;
        }
    }
    return -1;
}

}

//-------------------------

GoogleContactSqliteSyncAdaptor::GoogleContactSqliteSyncAdaptor(int accountId, GoogleTwoWayContactSyncAdaptor *parent)
    : QtContactsSqliteExtensions::TwoWayContactSyncAdaptor(accountId, qAppName(), *parent->m_contactManager)
    , q(parent)
{
}

GoogleContactSqliteSyncAdaptor::~GoogleContactSqliteSyncAdaptor()
{
}

bool GoogleContactSqliteSyncAdaptor::isLocallyDeletedGuid(const QString &guid) const
{
    if (guid.isEmpty()) {
        return false;
    }

    const TwoWayContactSyncAdaptorPrivate::ContactChanges &localChanges(d->m_localContactChanges[q->m_collection.id()]);
    for (const QContact &removedContact : localChanges.removedContacts) {
        if (guid == removedContact.detail<QContactGuid>().guid()) {
            return true;
        }
    }

    return false;
}

bool GoogleContactSqliteSyncAdaptor::determineRemoteCollections()
{
    if (q->m_collection.id().isNull()) {
        qCDebug(lcSocialPluginTrace) << "performing request to find My Contacts group with account" << q->m_accountId;
        q->requestData(GoogleTwoWayContactSyncAdaptor::ContactGroupRequest);
    } else {
        // we can just sync changes immediately
        qCDebug(lcSocialPluginTrace) << "requesting contact sync deltas with account" << q->m_accountId
                          << "for collection" << q->m_collection.id();
        remoteCollectionsDetermined(QList<QContactCollection>() << q->m_collection);
    }

    return true;
}

bool GoogleContactSqliteSyncAdaptor::deleteRemoteCollection(const QContactCollection &collection)
{
    qCWarning(lcSocialPlugin) << "Ignoring request to delete My Contacts collection" << collection.id();
    return true;
}

bool GoogleContactSqliteSyncAdaptor::determineRemoteContacts(const QContactCollection &collection)
{
    Q_UNUSED(collection)
    q->requestData(GoogleTwoWayContactSyncAdaptor::ContactRequest,
                   GoogleTwoWayContactSyncAdaptor::DetermineRemoteContacts);
    return true;
}

bool GoogleContactSqliteSyncAdaptor::determineRemoteContactChanges(const QContactCollection &collection,
                                                                   const QList<QContact> &localAddedContacts,
                                                                   const QList<QContact> &localModifiedContacts,
                                                                   const QList<QContact> &localDeletedContacts,
                                                                   const QList<QContact> &localUnmodifiedContacts,
                                                                   QContactManager::Error *error)
{
    Q_UNUSED(collection)
    Q_UNUSED(localAddedContacts)
    Q_UNUSED(localModifiedContacts)
    Q_UNUSED(localDeletedContacts)
    Q_UNUSED(localUnmodifiedContacts)
    Q_UNUSED(error)

    if (q->m_connectionsListParams.syncToken.isEmpty()) {
        // Notify the two-way sync adaptor that this is a full sync rather than a delta sync, so
        // that it will call determineRemoteContacts() to fetch all contacts for the collection.
        *error = QContactManager::NotSupportedError;
        return false;
    }

    q->requestData(GoogleTwoWayContactSyncAdaptor::ContactRequest,
                   GoogleTwoWayContactSyncAdaptor::DetermineRemoteContactChanges);
    return true;
}

bool GoogleContactSqliteSyncAdaptor::storeLocalChangesRemotely(const QContactCollection &collection,
                                                               const QList<QContact> &addedContacts,
                                                               const QList<QContact> &modifiedContacts,
                                                               const QList<QContact> &deletedContacts)
{
    Q_UNUSED(collection)

    q->upsyncLocalChanges(addedContacts, modifiedContacts, deletedContacts);
    return true;
}

void GoogleContactSqliteSyncAdaptor::storeRemoteChangesLocally(const QContactCollection &collection,
                                                               const QList<QContact> &addedContacts,
                                                               const QList<QContact> &modifiedContacts,
                                                               const QList<QContact> &deletedContacts)
{
    Q_UNUSED(collection)

    TwoWayContactSyncAdaptor::storeRemoteChangesLocally(q->m_collection, addedContacts, modifiedContacts, deletedContacts);
}

void GoogleContactSqliteSyncAdaptor::syncFinishedSuccessfully()
{
    qCInfo(lcSocialPlugin) << "Sync finished OK";

    q->syncFinished();
}

void GoogleContactSqliteSyncAdaptor::syncFinishedWithError()
{
    qCWarning(lcSocialPlugin) << "Sync finished with error";

    if (q->m_collection.id().isNull()) {
        return;
    }

    // If sync fails, clear the sync token and date for the collection, so that the next sync
    // requests a full contact listing, to ensure we are up-to-date with the server.

    q->m_collection.setExtendedMetaData(CollectionKeySyncToken, QString());
    q->m_collection.setExtendedMetaData(CollectionKeySyncTokenDate, QString());

    QHash<QContactCollection*, QList<QContact>* > modifiedCollections;
    QList<QContact> emptyContacts;
    modifiedCollections.insert(&q->m_collection, &emptyContacts);

    QtContactsSqliteExtensions::ContactManagerEngine *cme = QtContactsSqliteExtensions::contactManagerEngine(*q->m_contactManager);
    QContactManager::Error error = QContactManager::NoError;

    if (!cme->storeChanges(nullptr,
                          &modifiedCollections,
                          QList<QContactCollectionId>(),
                          QtContactsSqliteExtensions::ContactManagerEngine::PreserveLocalChanges,
                          true,
                          &error)) {
        qCWarning(lcSocialPlugin) << "Failed to clear sync token for account:" << q->m_accountId
                                  << "due to error:" << error;
    }
}

//-------------------------------------

GoogleTwoWayContactSyncAdaptor::GoogleTwoWayContactSyncAdaptor(QObject *parent)
    : GoogleDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::Contacts, parent)
    , m_contactManager(new QContactManager(QStringLiteral("org.nemomobile.contacts.sqlite")))
    , m_workerObject(new GoogleContactImageDownloader())
{
    connect(m_workerObject, &AbstractImageDownloader::imageDownloaded,
            this, &GoogleTwoWayContactSyncAdaptor::imageDownloaded);

    // can sync, enabled
    setInitialActive(true);
}

GoogleTwoWayContactSyncAdaptor::~GoogleTwoWayContactSyncAdaptor()
{
    delete m_workerObject;
}

QString GoogleTwoWayContactSyncAdaptor::syncServiceName() const
{
    return QStringLiteral("google-contacts");
}

void GoogleTwoWayContactSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    m_accountId = accountId;

    // Detect if this account was previously synced with the legacy Google Contacts API. If so,
    // remove all contacts and do a fresh sync with the Google People API.
    const QList<QContactCollection> collections = m_contactManager->collections();
    for (const QContactCollection &collection : collections) {
        if (collection.extendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_ACCOUNTID).toInt() == accountId
                && collection.extendedMetaData(QStringLiteral("atom-id")).isValid()) {
            qCInfo(lcSocialPlugin) << "Removing contacts synced with legacy Google Contacts API";
            purgeAccount(accountId);
        }
    }

    // Remove legacy settings file
    QString settingsFileName = QString::fromLatin1("%1/%2/gcontacts.ini")
            .arg(PRIVILEGED_DATA_DIR)
            .arg(QString::fromLatin1(SYNC_DATABASE_DIR));
    QFile::remove(settingsFileName);

    m_sqliteSync = new GoogleContactSqliteSyncAdaptor(accountId, this);

    // assume we can make up to 99 requests per sync, before being throttled.
    m_apiRequestsRemaining = 99;

    // call superclass impl.
    GoogleDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void GoogleTwoWayContactSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode )
{
    purgeAccount(oldId);
}

void GoogleTwoWayContactSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    if (accountId != m_accountId) {
        qCWarning(lcSocialPlugin) << "Cannot begin sync, expected account id" << m_accountId << "but got" << m_accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    m_accessToken = accessToken;

    // Find the Google contacts collection, if previously synced.
    m_collection = findCollection(*m_contactManager, accountId);
    if (m_collection.id().isNull()) {
        qCDebug(lcSocialPlugin) << "No MyContacts collection saved yet for account:" << accountId;
    } else {
        loadCollection(m_collection);
        qCDebug(lcSocialPlugin) << "Found MyContacts collection" << m_collection.id()
                                << "for account:" << accountId;
    }

    // Initialize the people.connections.list() parameters
    QString syncToken;
    if (!m_collection.id().isNull()) {
        syncToken = m_collection.extendedMetaData(CollectionKeySyncToken).toString();
        const QDateTime syncTokenDate = QDateTime::fromString(
                    m_collection.extendedMetaData(CollectionKeySyncTokenDate).toString(),
                    Qt::ISODate);
        // Google sync token expires after 7 days. If it's almost expired, request a new sync token
        // during this sync session.
        if (syncTokenDate.isValid()
                && syncTokenDate.daysTo(QDateTime::currentDateTimeUtc()) >= 6) {
            qCInfo(lcSocialPlugin) << "Will request new syncToken during this sync session";
            syncToken.clear();
        }
    }
    m_connectionsListParams.requestSyncToken = true;
    m_connectionsListParams.syncToken = syncToken;
    m_connectionsListParams.personFields = GooglePeople::Person::supportedPersonFields().join(',');

    // Start the sync
    if (!m_sqliteSync->startSync()) {
        m_sqliteSync->deleteLater();
        qCWarning(lcSocialPlugin) << "unable to start sync - aborting sync contacts with account:" << m_accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
    }
}

void GoogleTwoWayContactSyncAdaptor::requestData(
        DataRequestType requestType,
        ContactChangeNotifier contactChangeNotifier,
        const QString &pageToken)
{
    QUrl requestUrl;
    QUrlQuery urlQuery;
    if (requestType == ContactGroupRequest) {
        requestUrl = QUrl(QStringLiteral("https://people.googleapis.com/v1/contactGroups"));
        // Currently we do not add a syncToken for group requests, as we always fetch the complete
        // list.
    } else {
        requestUrl = QUrl(QStringLiteral("https://people.googleapis.com/v1/people/me/connections"));
        if (m_connectionsListParams.requestSyncToken) {
            urlQuery.addQueryItem(QStringLiteral("requestSyncToken"), QStringLiteral("true"));
        }
        if (!m_connectionsListParams.syncToken.isEmpty()) {
            urlQuery.addQueryItem(QStringLiteral("syncToken"),
                                  m_connectionsListParams.syncToken);
        }
        urlQuery.addQueryItem(QStringLiteral("personFields"),
                              m_connectionsListParams.personFields);
    }
    if (!pageToken.isEmpty()) {
        urlQuery.addQueryItem(QStringLiteral("pageToken"), pageToken);
    }
    requestUrl.setQuery(urlQuery);

    QNetworkRequest req(requestUrl);
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ") + m_accessToken).toUtf8());

    qCDebug(lcSocialPluginTrace) << "requesting" << requestUrl << "with account" << m_accountId;

    // we're requesting data.  Increment the semaphore so that we know we're still busy.
    incrementSemaphore(m_accountId);

    QNetworkReply *reply = m_networkAccessManager->get(req);
    if (reply) {
        reply->setProperty("requestType", requestType);
        reply->setProperty("contactChangeNotifier", contactChangeNotifier);
        reply->setProperty("accountId", m_accountId);
        if (requestType == ContactGroupRequest) {
            connect(reply, &QNetworkReply::finished,
                    this, &GoogleTwoWayContactSyncAdaptor::groupsFinishedHandler);
        } else {
            connect(reply, &QNetworkReply::finished,
                    this, &GoogleTwoWayContactSyncAdaptor::contactsFinishedHandler);
        }
        connect(reply, static_cast<void (QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error),
                this, &GoogleTwoWayContactSyncAdaptor::errorHandler);
        connect(reply, &QNetworkReply::sslErrors,
                this, &GoogleTwoWayContactSyncAdaptor::sslErrorsHandler);
        m_apiRequestsRemaining -= 1;
        setupReplyTimeout(m_accountId, reply);
    } else {
        qCWarning(lcSocialPlugin) << "unable to request data from Google account with id" << m_accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);
    }
}

void GoogleTwoWayContactSyncAdaptor::groupsFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    bool isError = reply->property("isError").toBool();
    reply->deleteLater();
    removeReplyTimeout(m_accountId, reply);

    if (isError) {
        qCWarning(lcSocialPlugin) << "error occurred when performing groups request for Google account" << m_accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);
        return;
    } else if (data.isEmpty()) {
        qCWarning(lcSocialPlugin) << "no groups data in reply from Google with account" << m_accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);
        return;
    }

    GooglePeopleApiResponse::ContactGroupsResponse response;
    if (!GooglePeopleApiResponse::readResponse(data, &response)) {
        qCWarning(lcSocialPlugin) << "unable to parse groups data from reply from Google using account with id" << m_accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);
        return;
    }

    qCDebug(lcSocialPluginTrace) << "received information about" << response.contactGroups.size()
                                 << "groups for account" << m_accountId;

    GooglePeople::ContactGroup myContactsGroup;
    for (auto it = response.contactGroups.constBegin(); it != response.contactGroups.constEnd(); ++it) {
        if (it->isMyContactsGroup()) {
            myContactsGroup = *it;
            break;
        }
    }

    if (!myContactsGroup.resourceName.isEmpty()) {
        // we can now continue with contact sync.
        m_collection = myContactsGroup.toCollection(m_accountId);
        m_sqliteSync->remoteCollectionsDetermined(QList<QContactCollection>() << m_collection);
    } else if (!response.nextPageToken.isEmpty()) {
        // request more groups if they exist.
        requestData(ContactGroupRequest, NoContactChangeNotifier, response.nextPageToken);

    } else {
        qCInfo(lcSocialPlugin) << "Cannot find My Contacts group when syncing Google contacts for account:" << m_accountId;
        m_sqliteSync->remoteCollectionsDetermined(QList<QContactCollection>());
    }

    decrementSemaphore(m_accountId);
}

void GoogleTwoWayContactSyncAdaptor::contactsFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());

    if (reply->error() == QNetworkReply::ProtocolInvalidOperationError) {
        QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 400
                && !m_retriedConnectionsList) {
            qCInfo(lcSocialPlugin) << "Will request new sync token, got error from server:"
                             << reply->readAll();
            DataRequestType requestType = static_cast<DataRequestType>(
                        reply->property("requestType").toInt());
            ContactChangeNotifier contactChangeNotifier = static_cast<ContactChangeNotifier>(
                        reply->property("contactChangeNotifier").toInt());
            m_connectionsListParams.requestSyncToken = true;
            m_connectionsListParams.syncToken.clear();
            m_retriedConnectionsList = true;
            requestData(requestType, contactChangeNotifier);
            decrementSemaphore(m_accountId);
            return;
        }
    }

    QByteArray data = reply->readAll();
    ContactChangeNotifier contactChangeNotifier = static_cast<ContactChangeNotifier>(reply->property("contactChangeNotifier").toInt());
    bool isError = reply->property("isError").toBool();
    reply->deleteLater();
    removeReplyTimeout(m_accountId, reply);

    if (isError) {
        qCWarning(lcSocialPlugin) << "error occurred when performing contacts request for Google account"
                          << m_accountId
                          << ", network error was:" << reply->error() << reply->errorString()
                          << "HTTP code:" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);
        return;
    } else if (data.isEmpty()) {
        qCWarning(lcSocialPlugin) << "no contact data in reply from Google with account" << m_accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);
        return;
    }

    GooglePeopleApiResponse::PeopleConnectionsListResponse response;
    if (!GooglePeopleApiResponse::readResponse(data, &response)) {
        qCWarning(lcSocialPlugin) << "unable to parse contacts data from reply from Google using account with id"
                          << m_accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);
        return;
    }

    if (!response.nextSyncToken.isEmpty()) {
        qCInfo(lcSocialPlugin) << "Received sync token for people.connections.list():"
                         << response.nextSyncToken;
        const QString dateString = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        m_collection.setExtendedMetaData(CollectionKeySyncToken, response.nextSyncToken);
        m_collection.setExtendedMetaData(CollectionKeySyncTokenDate, dateString);
    }

    QList<QContact> remoteAddModContacts;
    QList<QContact> remoteDelContacts;
    response.getContacts(m_accountId,
                         QList<QContactCollection>() << m_collection,
                         &remoteAddModContacts,
                         &remoteDelContacts);

    qCDebug(lcSocialPluginTrace) << "received information about"
                      << remoteAddModContacts.size() << "add/mod contacts and "
                      << remoteDelContacts.size() << "del contacts"
                      << "for account" << m_accountId;

    for (QContact c : remoteAddModContacts) {
        const QString guid = c.detail<QContactGuid>().guid();

        // get the saved etag
        const QString newEtag = GooglePeople::PersonMetadata::etag(c);
        if (newEtag.isEmpty()) {
            qCWarning(lcSocialPlugin) << "No etag found for contact:" << guid;
        } else if (newEtag == m_contactEtags.value(guid)) {
            // the etags match, so no remote changes have occurred.
            // most likely this is a spurious change, however it
            // may be the case that we have not yet downloaded the
            // avatar for this contact. Check this.
            QString remoteAvatarUrl;
            QString localAvatarFile;
            const QContactAvatar avatar = GooglePeople::Photo::getPrimaryPhoto(c, &remoteAvatarUrl, &localAvatarFile);

            if (!localAvatarFile.isEmpty() && !QFile::exists(localAvatarFile)) {
                // the avatar image has not yet been downloaded.
                qCDebug(lcSocialPlugin) << "Remote modification spurious except for missing avatar" << guid;
                m_contactAvatars.insert(guid, remoteAvatarUrl); // enqueue outstanding avatar.
            }
            if (m_connectionsListParams.syncToken.isEmpty()) {
                // This is a fresh sync, so keep the modification.
                qCDebug(lcSocialPlugin) << "Remote modification for contact:" << guid
                                        << "is not spurious, keeping it (this is a fresh sync)";
            } else {
                // This is a delta sync and the modification is spurious, so discard the contact.
                qCDebug(lcSocialPlugin) << "Disregarding spurious remote modification for contact:" << guid;
                continue;
            }
        }

        // put contact into added or modified list
        const QHash<QString, QString>::iterator contactIdIter = m_contactIds.find(guid);
        if (contactIdIter == m_contactIds.end()) {
            if (m_sqliteSync->isLocallyDeletedGuid(guid)) {
                qCDebug(lcSocialPluginTrace) << "New remote contact" << guid << "was locally deleted, ignoring";
            } else {
                m_remoteAdds.append(c);
                qCDebug(lcSocialPluginTrace) << "New remote contact" << guid;
            }
        } else {
            c.setId(QContactId::fromString(contactIdIter.value()));
            m_remoteMods.append(c);
            qCDebug(lcSocialPluginTrace) << "Found modified contact " << guid << ", etag now" << newEtag;
        }
    }

    for (auto it = remoteDelContacts.begin(); it != remoteDelContacts.end(); ++it) {
        QContact c = *it;
        const QString guid = c.detail<QContactGuid>().guid();
        const QString idStr = m_contactIds.value(guid);
        if (idStr.isEmpty()) {
            qCWarning(lcSocialPlugin) << "Unable to find deleted contact with guid: " << guid;
        } else {
            c.setId(QContactId::fromString(idStr));
            m_contactAvatars.remove(guid); // just in case the avatar was outstanding.
            m_remoteDels.append(c);
        }
    }

    if (!response.nextPageToken.isEmpty()) {
        // request more if they exist.
        qCDebug(lcSocialPluginTrace)
                << "more contact sync information is available server-side; performing another request with account"
                << m_accountId;
        requestData(ContactRequest, contactChangeNotifier, response.nextPageToken);
    } else {
        // we're finished downloading the remote changes - we should sync local changes up.
        qCInfo(lcSocialPlugin) << "Google contact sync with account" << m_accountId
                               << "got remote changes: A/M/R:"
                               << m_remoteAdds.count()
                               << m_remoteMods.count()
                               << m_remoteDels.count();

        continueSync(contactChangeNotifier);
    }

    decrementSemaphore(m_accountId);
}

void GoogleTwoWayContactSyncAdaptor::continueSync(ContactChangeNotifier contactChangeNotifier)
{
    // early out in case we lost connectivity
    if (syncAborted()) {
        qCWarning(lcSocialPlugin) << "aborting sync of account" << m_accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        // note: don't decrement here - it's done by contactsFinishedHandler().
        return;
    }

    // avatars of the added and modified contacts will need to be downloaded
    for (int i = 0; i < m_remoteAdds.size(); ++i) {
        addAvatarToDownload(&m_remoteAdds[i]);
    }
    for (int i = 0; i < m_remoteMods.size(); ++i) {
        addAvatarToDownload(&m_remoteMods[i]);
    }

    // now store the changes locally
    qCDebug(lcSocialPluginTrace) << "storing remote changes locally for account" << m_accountId;

    if (contactChangeNotifier == DetermineRemoteContactChanges) {
        m_sqliteSync->remoteContactChangesDetermined(m_collection,
                                                     m_remoteAdds,
                                                     m_remoteMods,
                                                     m_remoteDels);
    } else {
        m_sqliteSync->remoteContactsDetermined(m_collection, m_remoteAdds + m_remoteMods);
    }
}

void GoogleTwoWayContactSyncAdaptor::upsyncLocalChanges(const QList<QContact> &locallyAdded,
                                                        const QList<QContact> &locallyModified,
                                                        const QList<QContact> &locallyDeleted)
{
    QSet<QString> alreadyEncoded; // shouldn't be necessary, as determineLocalChanges should already ensure distinct result sets.
    for (const QContact &c : locallyDeleted) {
        const QString &guid = c.detail<QContactGuid>().guid();
        if (!guid.isEmpty()) {
            m_localDels.append(c);
            m_contactAvatars.remove(guid); // just in case the avatar was outstanding.
            alreadyEncoded.insert(guid);
        } else {
            qCInfo(lcSocialPlugin) << "Ignore locally-deleted contact" << c.id()
                                   << ", was not uploaded to server prior to local deletion";
        }
    }
    for (const QContact &c : locallyAdded) {
        const QString guid = c.detail<QContactGuid>().guid();
        if (!alreadyEncoded.contains(guid)) {
            m_localAdds.append(c);
            if (!guid.isEmpty()) {
                alreadyEncoded.insert(guid);
            }

            QString remoteAvatarUrl;
            QString localAvatarFile;
            GooglePeople::Photo::getPrimaryPhoto(c, &remoteAvatarUrl, &localAvatarFile);
            if (remoteAvatarUrl.isEmpty() && !localAvatarFile.isEmpty()) {
                // The avatar was created locally and needs to be uploaded.
                qCDebug(lcSocialPluginTrace) << "Will upsync avatar for new contact" << guid;
                m_localAvatarAdds.append(c);
            }
        }
    }
    for (const QContact &c : locallyModified) {
        const QString guid = c.detail<QContactGuid>().guid();
        if (!alreadyEncoded.contains(guid)) {
            m_localMods.append(c);

            // Determine the type of avatar change to be uploaded.
            QString remoteAvatarUrl;
            QString localAvatarFile;
            const QContactAvatar avatar = GooglePeople::Photo::getPrimaryPhoto(c, &remoteAvatarUrl, &localAvatarFile);
            const int changeFlag = avatar.value(QContactDetail__FieldChangeFlags).toInt();

            if (changeFlag & QContactDetail__ChangeFlag_IsDeleted) {
                qCDebug(lcSocialPluginTrace) << "Will upsync avatar deletion for contact" << guid;
                m_localAvatarDels.append(c);
            } else if ((changeFlag & QContactDetail__ChangeFlag_IsAdded)
                       || (changeFlag & QContactDetail__ChangeFlag_IsModified)) {
                if (localAvatarFile.isEmpty()) {
                    qCDebug(lcSocialPluginTrace) << "Will upsync avatar deletion for contact" << guid;
                    m_localAvatarDels.append(c);
                } else {
                    qCDebug(lcSocialPluginTrace) << "Will upsync avatar modification for contact" << guid;
                    // This is a local file, so upload it. The server will generate a remote image
                    // url for it and provide the url in the response, that we then can download.
                    // Note that the contact is added to m_localAvatarMods and not m_localAvatarAdds
                    // even if it is a new avatar file, because this is for an existing contact,
                    // not a new contact.
                    m_localAvatarMods.append(c);
                }
            }
        }
    }

    m_batchUpdateIndexes.clear();

    qCInfo(lcSocialPlugin) << "Google account:" << m_accountId <<
                     "upsyncing local contact A/M/R:"
                     << m_localAdds.count()  << "/"
                     << m_localMods.count()  << "/"
                     << m_localDels.count()
                     << "and local avatar A/M/R:"
                     << m_localAvatarAdds.count()  << "/"
                     << m_localAvatarMods.count()  << "/"
                     << m_localAvatarDels.count();

    upsyncLocalChangesList();
}

bool GoogleTwoWayContactSyncAdaptor::batchRemoteChanges(BatchedUpdate *batchedUpdate,
                                                        QList<QContact> *contacts,
                                                        GooglePeopleApi::OperationType updateType)
{
    int batchUpdateIndex = m_batchUpdateIndexes.value(updateType, contacts->count() - 1);

    while (batchUpdateIndex >= 0 && batchUpdateIndex < contacts->count()) {
        const QContact &contact = contacts->at(batchUpdateIndex--);
        m_batchUpdateIndexes[updateType] = batchUpdateIndex;
        batchedUpdate->batch[updateType].append(contact);
        batchedUpdate->batchCount++;

        if (batchUpdateIndex <= 0) {
            const QByteArray encodedContactUpdates =
                    GooglePeopleApiRequest::writeMultiPartRequest(batchedUpdate->batch);
            if (encodedContactUpdates.isEmpty()) {
                qCInfo(lcSocialPlugin) << "No data changes found, no non-avatar changes to upsync for contact"
                                 << contact.id() << "guid" << contact.detail<QContactGuid>().guid();
            } else {
                qCDebug(lcSocialPluginTrace) << "storing a batch of" << batchedUpdate->batchCount
                                  << "local changes to remote server for account" << m_accountId;
            }
            batchedUpdate->batch.clear();
            batchedUpdate->batchCount = 0;
            if (!encodedContactUpdates.isEmpty()) {
                storeToRemote(encodedContactUpdates);
                return true;
            }
        }
    }

    return false;
}

void GoogleTwoWayContactSyncAdaptor::upsyncLocalChangesList()
{
    bool postedData = false;
    if (!m_accountSyncProfile || m_accountSyncProfile->syncDirection() != Buteo::SyncProfile::SYNC_DIRECTION_FROM_REMOTE) {
        // two-way sync is the default setting.  Upsync the changes.
        BatchedUpdate batch;
        if (!postedData) {
            postedData = batchRemoteChanges(&batch, &m_localAdds, GooglePeopleApi::CreateContact);
        }
        if (!postedData) {
            postedData = batchRemoteChanges(&batch, &m_localMods, GooglePeopleApi::UpdateContact);
        }
        if (!postedData) {
            postedData = batchRemoteChanges(&batch, &m_localDels, GooglePeopleApi::DeleteContact);
        }
        if (!postedData) {
            // The avatar additions must be sent after the CreateContact calls, so that we have a
            // valid Person resourceName to attach to the UpdateContactPhoto call.
            postedData = batchRemoteChanges(&batch, &m_localAvatarAdds, GooglePeopleApi::AddContactPhoto);
        }
        if (!postedData) {
            postedData = batchRemoteChanges(&batch, &m_localAvatarMods, GooglePeopleApi::UpdateContactPhoto);
        }
        if (!postedData) {
            postedData = batchRemoteChanges(&batch, &m_localAvatarDels, GooglePeopleApi::DeleteContactPhoto);
        }
    } else {
        qCInfo(lcSocialPlugin) << "skipping upload of local contacts changes due to profile direction setting for account" << m_accountId;
    }

    if (!postedData) {
        qCInfo(lcSocialPlugin) << "All upsync requests sent";

        // Nothing left to upsync.
        // notify TWCSA that the upsync is complete.
        m_sqliteSync->localChangesStoredRemotely(m_collection, m_localAdds, m_localMods);
    }
}

void GoogleTwoWayContactSyncAdaptor::storeToRemote(const QByteArray &encodedContactUpdates)
{
    QUrl requestUrl(QLatin1String("https://people.googleapis.com/batch"));
    QNetworkRequest req(requestUrl);
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ") + m_accessToken).toUtf8());
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ") + m_accessToken).toUtf8());
    req.setRawHeader(QString(QLatin1String("Content-Type")).toUtf8(),
                     QString(QLatin1String("multipart/mixed; boundary=\"batch_people\"")).toUtf8());
    req.setHeader(QNetworkRequest::ContentLengthHeader, encodedContactUpdates.size());

    // we're posting data.  Increment the semaphore so that we know we're still busy.
    incrementSemaphore(m_accountId);
    QNetworkReply *reply = m_networkAccessManager->post(req, encodedContactUpdates);
    if (reply) {
        connect(reply, &QNetworkReply::finished,
                this, &GoogleTwoWayContactSyncAdaptor::postFinishedHandler);
        connect(reply, static_cast<void (QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error),
                this, &GoogleTwoWayContactSyncAdaptor::postErrorHandler);
        connect(reply, &QNetworkReply::sslErrors,
                this, &GoogleTwoWayContactSyncAdaptor::postErrorHandler);
        m_apiRequestsRemaining -= 1;
        setupReplyTimeout(m_accountId, reply);
    } else {
        qCWarning(lcSocialPlugin) << "unable to post contacts to Google account with id" << m_accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);
    }
}

void GoogleTwoWayContactSyncAdaptor::postFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray response = reply->readAll();
    reply->deleteLater();
    removeReplyTimeout(m_accountId, reply);

    if (reply->property("isError").toBool()) {
        qCWarning(lcSocialPlugin) << "error occurred posting contact data to google with account" << m_accountId << "," <<
                          "got response:" << QString::fromUtf8(response);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);
        return;
    }

    QList <GooglePeopleApiResponse::BatchResponsePart> operationResponses;
    if (!GooglePeopleApiResponse::readMultiPartResponse(response, &operationResponses)) {
        qCWarning(lcSocialPlugin) << "unable to read response for batch operation with Google account" << m_accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);
        return;
    }

    const QList<QContactCollection> collections { m_collection };

    bool errorOccurredInBatch = false;

    for (const GooglePeopleApiResponse::BatchResponsePart &response : operationResponses) {
        GooglePeopleApi::OperationType operationType;
        QString contactIdString;
        GooglePeople::Person person;
        GooglePeopleApiResponse::BatchResponsePart::Error error;
        response.parse(&operationType, &contactIdString, &person, &error);

        if (!error.status.isEmpty()) {
            if (error.code == 404
                    && (operationType == GooglePeopleApi::DeleteContact
                        || operationType == GooglePeopleApi::DeleteContactPhoto)) {
                // Couldn't find the remote contact or photo to be deleted; perhaps some previous
                // change was not synced as expected. This is not a problem as we will just delete
                // it locally.
                qCInfo(lcSocialPlugin) << "Unable to delete contact or photo on the server, will just delete it locally."
                                 << "id:" << contactIdString
                                 << "resource:" << person.resourceName;
            } else {
                errorOccurredInBatch = true;
                qCWarning(lcSocialPlugin) << "batch operation error:\n"
                                  "    contentId:     " << response.contentId << "\n"
                                  "    error.code:   " << error.code << "\n"
                                  "    error.message: " << error.message << "\n"
                                  "    error.status:  " << error.status << "\n";
            }
        }

        if (errorOccurredInBatch) {
            // The sync will finish with an error. Keep looking for other possible errors, but
            // don't process any more responses.
            continue;
        }

        qCDebug(lcSocialPluginTrace) << "Process response for batched request" << response.contentId
                          << "status =" << response.bodyStatusLine
                          << "body len =" << response.body.length();
        if (!person.resourceName.isEmpty()) {
            qCDebug(lcSocialPlugin) << "Batched response contains Person(resourceName ="
                              << person.resourceName << ")";
        }

        // Save contact etag and other details into the added/modified lists so that the
        // updated details are saved into the database later.
        QList<QContact> *contactList = nullptr;
        switch (operationType) {
        case GooglePeopleApi::CreateContact:
        case GooglePeopleApi::AddContactPhoto:
            contactList = &m_localAdds;
            break;
        case GooglePeopleApi::UpdateContact:
        case GooglePeopleApi::UpdateContactPhoto:
        case GooglePeopleApi::DeleteContactPhoto:
            contactList = &m_localMods;
            break;
        case GooglePeopleApi::DeleteContact:
            // Nothing to do, the response body will be empty.
            break;
        case GooglePeopleApi::UnsupportedOperation:
            break;
        }

        if (contactList) {
            if (!person.isValid()) {
                qCWarning(lcSocialPlugin) << "Cannot read Person object!";
                qCDebug(lcSocialPluginTrace) << "Response data was:" << response.body;
                continue;
            }

            const QContactId contactId = QContactId::fromString(contactIdString);
            const int listIndex = indexOfContact(*contactList, contactId);
            if (listIndex < 0) {
                qCWarning(lcSocialPlugin) << "Cannot save details, contact" << contactId.toString()
                                  << " not found in added/modified contacts";
                continue;
            }

            QContact *contact = &((*contactList)[listIndex]);
            if (!person.saveToContact(contact, m_accountId, collections)) {
                qCWarning(lcSocialPlugin) << "Cannot save added/modified details for contact"
                                  << contactId.toString();
                continue;
            }

            if (operationType == GooglePeopleApi::CreateContact) {
                // The contact has now been assigned a resourceName from the Google server.
                // If the contact has an avatar to be uploaded in a later batch, update the
                // guid for the contact in m_localAvatarAdds to ensure the resourceName is
                // valid when the avatar is uploaded.
                const int avatarAddIndex = indexOfContact(m_localAvatarAdds, contact->id());
                if (avatarAddIndex >= 0) {
                    QContactGuid guid = contact->detail<QContactGuid>();
                    m_localAvatarAdds[avatarAddIndex].saveDetail(&guid);
                }
            } else if (operationType == GooglePeopleApi::AddContactPhoto
                       || operationType == GooglePeopleApi::UpdateContactPhoto) {
                // When a contact photo is uploaded to the server, the person's "photos" is
                // updated with a new remote url for the avatar; add this url to the list of
                // avatars to be downloaded later.
                addAvatarToDownload(contact);
            }
        }
    }

    if (errorOccurredInBatch) {
        qCWarning(lcSocialPlugin) << "error occurred during batch operation with Google account" << m_accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
    } else {
        // continue with more, if there were more than one page of updates to post.
        upsyncLocalChangesList();
    }

    // finished with this request, so decrementing semaphore.
    decrementSemaphore(m_accountId);
}

void GoogleTwoWayContactSyncAdaptor::postErrorHandler()
{
    sender()->setProperty("isError", QVariant::fromValue<bool>(true));
}

void GoogleTwoWayContactSyncAdaptor::syncFinished()
{
    // If this is the first sync, TWCSA will have saved the collection and given it a valid id, so
    // update collection so that any post-sync operations (e.g. saving of queued avatar downloads)
    // will refer to a valid collection.
    if (m_collection.id().isNull()) {
        const QContactCollection savedCollection = findCollection(*m_contactManager, m_accountId);
        if (savedCollection.id().isNull()) {
            qCWarning(lcSocialPlugin) << "Error: cannot find saved My Contacts collection!";
        } else {
            m_collection.setId(savedCollection.id());
        }
    }

    // Attempt to download any outstanding avatars.
    queueOutstandingAvatars();
}

void GoogleTwoWayContactSyncAdaptor::queueOutstandingAvatars()
{
    int queuedCount = 0;
    for (QHash<QString, QString>::const_iterator it = m_contactAvatars.constBegin();
            it != m_contactAvatars.constEnd(); ++it) {
        if (!it.value().isEmpty() && queueAvatarForDownload(it.key(), it.value())) {
            queuedCount++;
        }
    }

    qCDebug(lcSocialPluginTrace) << "queued" << queuedCount << "outstanding avatars for download for account"
                      << m_accountId;
}

bool GoogleTwoWayContactSyncAdaptor::queueAvatarForDownload(const QString &contactGuid, const QString &imageUrl)
{
    if (m_apiRequestsRemaining > 0 && !m_queuedAvatarsForDownload.contains(contactGuid)) {
        m_apiRequestsRemaining -= 1;
        m_queuedAvatarsForDownload[contactGuid] = imageUrl;

        QVariantMap metadata;
        metadata.insert(IMAGE_DOWNLOADER_TOKEN_KEY, m_accessToken);
        metadata.insert(IMAGE_DOWNLOADER_IDENTIFIER_KEY, contactGuid);
        incrementSemaphore(m_accountId);
        QMetaObject::invokeMethod(m_workerObject, "queue", Qt::QueuedConnection,
                                  Q_ARG(QString, imageUrl), Q_ARG(QVariantMap, metadata));

        return true;
    }

    return false;
}

bool GoogleTwoWayContactSyncAdaptor::addAvatarToDownload(QContact *contact)
{
    // The avatar detail from the remote contact will be of the form:
    // https://<host>.googleusercontent.com/<some generated path>/photo.jpg"
    // (The server will generate a new URL whenever the photo content changes, so there is no need
    // to store a photo etag to track changes.)
    // If the remote URL has changed, or the file has not been downloaded, then add it to the
    // list of pending avatar downloads.

    if (!contact) {
        return false;
    }

    const QString contactGuid = contact->detail<QContactGuid>().guid();
    if (contactGuid.isEmpty()) {
        return false;
    }

    QString remoteAvatarUrl;
    QString localAvatarFile;
    const QContactAvatar avatar = GooglePeople::Photo::getPrimaryPhoto(
                *contact, &remoteAvatarUrl, &localAvatarFile);

    const QPair<QString,QString> prevAvatar = m_previousAvatarUrls.value(contactGuid);
    const QString prevRemoteAvatarUrl = prevAvatar.first;
    const QString prevLocalAvatarFile = prevAvatar.second;

    const bool isNewAvatar = prevRemoteAvatarUrl.isEmpty();
    const bool isModifiedAvatar = !isNewAvatar && prevRemoteAvatarUrl != remoteAvatarUrl;
    const bool isMissingFile = !QFile::exists(localAvatarFile);

    if (!isNewAvatar && !isModifiedAvatar && !isMissingFile) {
        // No need to download the file.
        return false;
    }

    if (!prevLocalAvatarFile.isEmpty()) {
        QFile::remove(prevLocalAvatarFile);
    }

    // queue outstanding avatar for download once all upsyncs are complete
    m_contactAvatars.insert(contactGuid, remoteAvatarUrl);

    return true;
}

void GoogleTwoWayContactSyncAdaptor::imageDownloaded(const QString &url, const QString &path,
                                                     const QVariantMap &metadata)
{
    // Load finished, update the avatar, decrement semaphore
    QString contactGuid = metadata.value(IMAGE_DOWNLOADER_IDENTIFIER_KEY).toString();

    // Empty path signifies that an error occurred.
    if (path.isEmpty()) {
        qCWarning(lcSocialPlugin) << "Unable to download avatar" << url;
    } else {
        // no longer outstanding.
        m_contactAvatars.remove(contactGuid);
        m_queuedAvatarsForDownload.remove(contactGuid);
    }

    decrementSemaphore(m_accountId);
}

void GoogleTwoWayContactSyncAdaptor::purgeAccount(int pid)
{
    QtContactsSqliteExtensions::ContactManagerEngine *cme = QtContactsSqliteExtensions::contactManagerEngine(*m_contactManager);
    QContactManager::Error error = QContactManager::NoError;

    QList<QContactCollection> addedCollections;
    QList<QContactCollection> modifiedCollections;
    QList<QContactCollection> deletedCollections;
    QList<QContactCollection> unmodifiedCollections;

    if (!cme->fetchCollectionChanges(pid,
                                     qAppName(),
                                     &addedCollections,
                                     &modifiedCollections,
                                     &deletedCollections,
                                     &unmodifiedCollections,
                                     &error)) {
        qCWarning(lcSocialPlugin) << "Cannot find collection for account" << pid << "error:" << error;
        return;
    }

    const QList<QContactCollection> collections = addedCollections + modifiedCollections + deletedCollections + unmodifiedCollections;
    if (collections.isEmpty()) {
        qCInfo(lcSocialPlugin) << "Nothing to purge, no collection has been saved for account" << pid;
        return;
    }

    for (const QContactCollection &collection : collections) {
        // Delete local avatar image files.
        QContactCollectionFilter collectionFilter;
        collectionFilter.setCollectionId(collection.id());
        QContactFetchHint fetchHint;
        fetchHint.setOptimizationHints(QContactFetchHint::NoRelationships);
        fetchHint.setDetailTypesHint(QList<QContactDetail::DetailType>()
                                     << QContactDetail::TypeGuid
                                     << QContactDetail::TypeAvatar);
        const QList<QContact> savedContacts = m_contactManager->contacts(collectionFilter, QList<QContactSortOrder>(), fetchHint);
        for (const QContact &contact : savedContacts) {
            const QList<QContactAvatar> avatars = contact.details<QContactAvatar>();
            for (const QContactAvatar &avatar : avatars) {
                const QString localFilePath = avatar.imageUrl().toString();
                if (!localFilePath.isEmpty() && !QFile::remove(localFilePath)) {
                    qCWarning(lcSocialPlugin) << "Failed to remove avatar:" << localFilePath;
                }
            }
        }
    }

    QList<QContactCollectionId> collectionIds;
    for (const QContactCollection &collection : collections) {
        collectionIds.append(collection.id());
    }

    // Delete the collection and its contacts.
    if (cme->storeChanges(nullptr,
                          nullptr,
                          collectionIds,
                          QtContactsSqliteExtensions::ContactManagerEngine::PreserveLocalChanges,
                          true,
                          &error)) {
        qCInfo(lcSocialPlugin) << "purged account" << pid << "and successfully removed collections" << collectionIds;
    } else {
        qCWarning(lcSocialPlugin) << "Failed to remove My Contacts collection during purge of account" << pid
                          << "error:" << error;
    }
}

void GoogleTwoWayContactSyncAdaptor::finalize(int accountId)
{
    if (syncAborted()|| status() == SocialNetworkSyncAdaptor::Error) {
        m_sqliteSync->syncFinishedWithError();
        return;
    }

    if (accountId != m_accountId
            || m_accessToken.isEmpty()) {
        // account failure occurred before sync process was started,
        // in this case we have nothing left to do except cleanup.
        return;
    }

    // sync was successful, allow cleaning up contacts from removed accounts.
    m_allowFinalCleanup = true;
}

void GoogleTwoWayContactSyncAdaptor::finalCleanup()
{
    // Only perform the cleanup if the sync cycle was successful.
    // Note: purgeDataForOldAccount() will still be invoked by Buteo
    // in response to the account being deleted when restoring the
    // backup, so we cannot avoid the problem of "lost contacts"
    // completely.  See JB#38210 for more information.
    if (!m_allowFinalCleanup) {
        return;
    }

    // Synchronously find any contacts which need to be removed,
    // which were somehow "left behind" by the sync process.

    // first, get a list of all existing google account ids
    QList<int> googleAccountIds;
    QList<int> purgeAccountIds;
    QList<int> currentAccountIds;
    QList<uint> uaids = m_accountManager->accountList();
    Q_FOREACH (uint uaid, uaids) {
        currentAccountIds.append(static_cast<int>(uaid));
    }
    for (int currId : currentAccountIds) {
        Accounts::Account *act = Accounts::Account::fromId(m_accountManager, currId, this);
        if (act) {
            if (act->providerName() == QString(QLatin1String("google"))) {
                // this account still exists, no need to purge its content.
                googleAccountIds.append(currId);
            }
            act->deleteLater();
        }
    }

    // find all account ids from which contacts have been synced
    const QList<QContactCollection> collections = m_contactManager->collections();
    for (const QContactCollection &collection : collections) {
        if (GooglePeople::ContactGroup::isMyContactsCollection(collection)) {
            const int purgeId = collection.extendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_ACCOUNTID).toInt();
            if (purgeId && !googleAccountIds.contains(purgeId) && !purgeAccountIds.contains(purgeId)) {
                // this account no longer exists, and needs to be purged.
                purgeAccountIds.append(purgeId);
            }
        }
    }

    // purge all data for those account ids which no longer exist.
    if (purgeAccountIds.size()) {
        qCInfo(lcSocialPlugin) << "finalCleanup() purging contacts from" << purgeAccountIds.size() << "non-existent Google accounts";
        for (int purgeId : purgeAccountIds) {
            purgeAccount(purgeId);
        }
    }
}

void GoogleTwoWayContactSyncAdaptor::loadCollection(const QContactCollection &collection)
{
    QContactCollectionFilter collectionFilter;
    collectionFilter.setCollectionId(collection.id());
    QContactFetchHint noRelationships;
    noRelationships.setOptimizationHints(QContactFetchHint::NoRelationships);
    QList<QContact> savedContacts = m_contactManager->contacts(collectionFilter, QList<QContactSortOrder>(), noRelationships);

    for (const QContact &contact : savedContacts) {
        const QString contactGuid = contact.detail<QContactGuid>().guid();
        if (contactGuid.isEmpty()) {
            qCDebug(lcSocialPlugin) << "No guid found for saved contact, must be new:" << contact.id();
            continue;
        }

        // m_contactEtags
        const QString etag = GooglePeople::PersonMetadata::etag(contact);
        if (!etag.isEmpty()) {
            m_contactEtags[contactGuid] = etag;
        }

        // m_contactIds
        m_contactIds[contactGuid] = contact.id().toString();

        // m_avatarImageUrls
        QString remoteAvatarUrl;
        QString localAvatarFile;
        GooglePeople::Photo::getPrimaryPhoto(contact, &remoteAvatarUrl, &localAvatarFile);
        m_previousAvatarUrls.insert(contactGuid, qMakePair(remoteAvatarUrl,localAvatarFile));
    }
}
