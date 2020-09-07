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
#include "googlecontactstream.h"
#include "googlecontactatom.h"
#include "googlecontactimagedownloader.h"

#include "constants_p.h"
#include "trace.h"

#include <twowaycontactsyncadaptor_impl.h>
#include <qtcontacts-extensions_manager_impl.h>
#include <qcontactstatusflags_impl.h>

#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QFile>
#include <QtCore/QByteArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QSettings>
#include <QtGui/QImageReader>

#include <QtContacts/QContactCollectionFilter>
#include <QtContacts/QContactIntersectionFilter>
#include <QtContacts/QContact>
#include <QtContacts/QContactSyncTarget>
#include <QtContacts/QContactGuid>
#include <QtContacts/QContactName>
#include <QtContacts/QContactNickname>
#include <QtContacts/QContactAvatar>
#include <QtContacts/QContactUrl>
#include <QtContacts/QContactGender>
#include <QtContacts/QContactNote>
#include <QtContacts/QContactBirthday>
#include <QtContacts/QContactPhoneNumber>
#include <QtContacts/QContactEmailAddress>

#include <Accounts/Manager>
#include <Accounts/Account>

#define SOCIALD_GOOGLE_MAX_CONTACT_ENTRY_RESULTS 50

static const char *IMAGE_DOWNLOADER_TOKEN_KEY = "url";
static const char *IMAGE_DOWNLOADER_ACCOUNT_ID_KEY = "account_id";
static const char *IMAGE_DOWNLOADER_IDENTIFIER_KEY = "identifier";

namespace {

const QString MyContactsCollectionName = QStringLiteral("google-MyContacts");
const QString CollectionKeyLastSync = QStringLiteral("last-sync-time");
const QString CollectionKeyAtomId = QStringLiteral("atom-id");

QContactCollection findCollection(const QContactManager &contactManager, const QString &name, int accountId)
{
    const QList<QContactCollection> collections = contactManager.collections();
    for (const QContactCollection &collection : collections) {
        if (collection.metaData(QContactCollection::KeyName).toString() == name
                && collection.extendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_ACCOUNTID).toInt() == accountId) {
            return collection;
        }
    }
    return QContactCollection();
}

QContact findContact(const QList<QContact> &contacts, const QString &guid)
{
    for (const QContact &contact : contacts) {
        if (contact.detail<QContactGuid>().guid() == guid) {
            return contact;
        }
    }
    return QContact();
}

QString collectionAtomId(const QContactCollection &collection)
{
    return collection.extendedMetaData(CollectionKeyAtomId).toString();
}

}

//-------------------------

GoogleContactSqliteSyncAdaptor::GoogleContactSqliteSyncAdaptor(int accountId, GoogleTwoWayContactSyncAdaptor *parent)
    : QtContactsSqliteExtensions::TwoWayContactSyncAdaptor(accountId, qAppName(), *parent->m_contactManager)
    , q(parent)
    , m_accountId(accountId)
{
    m_collection = findCollection(contactManager(), MyContactsCollectionName, m_accountId);
    if (m_collection.id().isNull()) {
        SOCIALD_LOG_DEBUG("No MyContacts collection saved yet for account:" << m_accountId);
    } else {
        SOCIALD_LOG_DEBUG("Found MyContacts collection" << m_collection.id() << "for account:" << m_accountId);
    }
}

GoogleContactSqliteSyncAdaptor::~GoogleContactSqliteSyncAdaptor()
{
}

int GoogleContactSqliteSyncAdaptor::accountId() const
{
    return m_accountId;
}

bool GoogleContactSqliteSyncAdaptor::determineRemoteCollections()
{
    if (collectionAtomId(m_collection).isEmpty()) {
        // we need to determine the atom id of the My Contacts group
        // because we upload newly added contacts to that group.
        SOCIALD_LOG_TRACE("performing request to determine atom id of My Contacts group with account" << m_accountId);
        q->requestData(m_accountId, 0, QString(), QDateTime(), GoogleTwoWayContactSyncAdaptor::ContactGroupRequest);
    } else {
        // we can just sync changes immediately
        SOCIALD_LOG_TRACE("atom id of My Contacts group already known:" << collectionAtomId(m_collection)
                          << "requesting contact sync deltas with account" << m_accountId);
        remoteCollectionsDetermined(QList<QContactCollection>() << m_collection);
    }

    return true;
}

bool GoogleContactSqliteSyncAdaptor::deleteRemoteCollection(const QContactCollection &collection)
{
    SOCIALD_LOG_ERROR("Ignoring request to delete My Contacts collection" << collection.id());
    return true;
}

bool GoogleContactSqliteSyncAdaptor::determineRemoteContacts(const QContactCollection &collection)
{
    q->requestData(m_accountId,
                   0,
                   QString(),
                   collection.extendedMetaData(CollectionKeyLastSync).toDateTime(),
                   GoogleTwoWayContactSyncAdaptor::ContactRequest,
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
    Q_UNUSED(localAddedContacts)
    Q_UNUSED(localModifiedContacts)
    Q_UNUSED(localDeletedContacts)
    Q_UNUSED(localUnmodifiedContacts)
    Q_UNUSED(error)

    q->requestData(m_accountId,
                   0,
                   QString(),
                   collection.extendedMetaData(CollectionKeyLastSync).toDateTime(),
                   GoogleTwoWayContactSyncAdaptor::ContactRequest,
                   GoogleTwoWayContactSyncAdaptor::DetermineRemoteContactChanges);
    return true;
}

bool GoogleContactSqliteSyncAdaptor::storeLocalChangesRemotely(const QContactCollection &collection,
                                                               const QList<QContact> &addedContacts,
                                                               const QList<QContact> &modifiedContacts,
                                                               const QList<QContact> &deletedContacts)
{
    const QDateTime since = collection.extendedMetaData(CollectionKeyLastSync).toDateTime();
    q->upsyncLocalChanges(since, addedContacts, modifiedContacts, deletedContacts, m_accountId);
    return true;
}

void GoogleContactSqliteSyncAdaptor::storeRemoteChangesLocally(const QContactCollection &collection,
                                                               const QList<QContact> &addedContacts,
                                                               const QList<QContact> &modifiedContacts,
                                                               const QList<QContact> &deletedContacts)
{
    Q_UNUSED(collection)

    // Store state changes to the collection
    q->storeSyncStateData(&m_collection);

    TwoWayContactSyncAdaptor::storeRemoteChangesLocally(m_collection, addedContacts, modifiedContacts, deletedContacts);
}

void GoogleContactSqliteSyncAdaptor::syncFinishedSuccessfully()
{
    SOCIALD_LOG_DEBUG("Sync finished OK");

    // If this is the first sync, TWCSA will have saved the collection and given it a valid id, so
    // update m_collection so that any post-sync operations (e.g. saving of queued avatar downloads)
    // will refer to a valid collection.
    const QContactCollection savedCollection = findCollection(contactManager(), MyContactsCollectionName, m_accountId);
    if (savedCollection.id().isNull()) {
        SOCIALD_LOG_DEBUG("Error: cannot find saved My Contacts collection!");
    } else {
        m_collection.setId(savedCollection.id());
    }
}

void GoogleContactSqliteSyncAdaptor::syncFinishedWithError()
{
    SOCIALD_LOG_DEBUG("Sync finished with error");
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
    // check if we need to perform a complete clean sync
    QString settingsFileName = QString::fromLatin1("%1/%2/gcontacts.ini")
            .arg(PRIVILEGED_DATA_DIR)
            .arg(QString::fromLatin1(SYNC_DATABASE_DIR));
    QSettings settingsFile(settingsFileName, QSettings::IniFormat);
    bool doneCleanSync = settingsFile.value(QString::fromLatin1("%1-cleansync").arg(accountId), QVariant::fromValue<bool>(false)).toBool();
    if (!doneCleanSync) {
        SOCIALD_LOG_INFO("Performing clean sync of Google contacts from account:" << accountId);
        purgeAccount(accountId); // purge all data for the account before syncing
        settingsFile.setValue(QString::fromLatin1("%1-cleansync").arg(accountId), QVariant::fromValue<bool>(true));
        settingsFile.sync();
    }

    // assume we can make up to 99 requests per sync, before being throttled.
    m_apiRequestsRemaining[accountId] = 99;

    // call superclass impl.
    GoogleDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void GoogleTwoWayContactSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode )
{
    purgeAccount(oldId);
}

void GoogleTwoWayContactSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    Accounts::Account *account = Accounts::Account::fromId(m_accountManager, accountId, this);
    if (!account) {
        SOCIALD_LOG_ERROR("unable to load Google account" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    account->selectService(Accounts::Service());
    QString emailAddress = account->valueAsString(QStringLiteral("default_credentials_username"));
    if (emailAddress.isEmpty()) {
        emailAddress = account->valueAsString(QStringLiteral("name"));
    }
    account->deleteLater();
    if (emailAddress.isEmpty()) {
        SOCIALD_LOG_ERROR("unable to determine email address for Google account" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    // clear our cache lists if necessary.
    m_remoteAdds[accountId].clear();
    m_remoteMods[accountId].clear();
    m_remoteDels[accountId].clear();
    m_localAdds[accountId].clear();
    m_localMods[accountId].clear();
    m_localDels[accountId].clear();
    m_accessTokens[accountId] = accessToken;
    m_emailAddresses[accountId] = emailAddress;

    GoogleContactSqliteSyncAdaptor *sqliteSync = m_sqliteSync.value(accountId);
    if (sqliteSync) {
        delete sqliteSync;
    }
    sqliteSync = new GoogleContactSqliteSyncAdaptor(accountId, this);
    readSyncStateData(sqliteSync->m_collection);

    if (!sqliteSync->startSync()) {
        sqliteSync->deleteLater();
        SOCIALD_LOG_ERROR("unable to start sync - aborting sync contacts with account:" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    m_sqliteSync[accountId] = sqliteSync;
}

void GoogleTwoWayContactSyncAdaptor::requestData(int accountId, int startIndex, const QString &continuationRequest, const QDateTime &syncTimestamp, DataRequestType requestType, ContactChangeNotifier contactChangeNotifier)
{
    const QString accessToken = m_accessTokens[accountId];
    QUrl requestUrl;
    if (continuationRequest.isEmpty()) {
        QUrlQuery urlQuery;
        if (requestType == ContactGroupRequest) {
            requestUrl = QUrl(QStringLiteral("https://www.google.com/m8/feeds/groups/default/full"));
        } else {
            requestUrl = QUrl(QStringLiteral("https://www.google.com/m8/feeds/contacts/default/full/"));
            if (!syncTimestamp.isNull()) { // delta query
                urlQuery.addQueryItem("updated-min", syncTimestamp.toString(Qt::ISODate));
                urlQuery.addQueryItem("showdeleted", QStringLiteral("true"));
            }
        }
        if (startIndex >= 1) {
            urlQuery.addQueryItem ("start-index", QString::number(startIndex));
        }
        urlQuery.addQueryItem("max-results", QString::number(SOCIALD_GOOGLE_MAX_CONTACT_ENTRY_RESULTS));
        requestUrl.setQuery(urlQuery);
    } else {
        requestUrl = QUrl(continuationRequest);
    }

    QNetworkRequest req(requestUrl);
    req.setRawHeader("GData-Version", "3.0");
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ") + accessToken).toUtf8());

    SOCIALD_LOG_TRACE("requesting" << requestUrl << "with start index" << startIndex << "with account" << accountId);

    // we're requesting data.  Increment the semaphore so that we know we're still busy.
    incrementSemaphore(accountId);
    QNetworkReply *reply = m_networkAccessManager->get(req);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("continuationRequest", continuationRequest);
        reply->setProperty("lastSyncTimestamp", syncTimestamp);
        reply->setProperty("startIndex", startIndex);
        reply->setProperty("contactChangeNotifier", contactChangeNotifier);
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
        m_apiRequestsRemaining[accountId] = m_apiRequestsRemaining[accountId] - 1;
        setupReplyTimeout(accountId, reply);
    } else {
        SOCIALD_LOG_ERROR("unable to request data from Google account with id" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
    }
}

void GoogleTwoWayContactSyncAdaptor::groupsFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int startIndex = reply->property("startIndex").toInt();
    int accountId = reply->property("accountId").toInt();
    QDateTime lastSyncTimestamp = reply->property("lastSyncTimestamp").toDateTime();
    bool isError = reply->property("isError").toBool();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    if (isError) {
        SOCIALD_LOG_ERROR("error occurred when performing groups request for Google account" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    } else if (data.isEmpty()) {
        SOCIALD_LOG_ERROR("no groups data in reply from Google with account" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    GoogleContactStream parser(false, accountId);
    GoogleContactAtom *atom = parser.parse(data);

    if (!atom) {
        SOCIALD_LOG_ERROR("unable to parse groups data from reply from Google using account with id" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    SOCIALD_LOG_TRACE("received information about" << atom->entrySystemGroups().size() << "groups for account" << accountId);

    if (atom->entrySystemGroups().contains(QStringLiteral("Contacts"))) {
        QContactCollection collection;
        collection.setMetaData(QContactCollection::KeyName, MyContactsCollectionName);
        collection.setMetaData(QContactCollection::KeyDescription, QStringLiteral("Google - Contacts"));
        collection.setMetaData(QContactCollection::KeyColor, QStringLiteral("tomato"));
        collection.setMetaData(QContactCollection::KeySecondaryColor, QStringLiteral("royalblue"));
        collection.setExtendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_APPLICATIONNAME, QCoreApplication::applicationName());
        collection.setExtendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_ACCOUNTID, accountId);

        // we have found the atom id of the group we need to upload new contacts to.
        QString myContactsGroupAtomId = atom->entrySystemGroups().value(QStringLiteral("Contacts"));
        if (myContactsGroupAtomId.isEmpty()) {
            // We don't consider this a fatal error,
            // instead, we just refuse to upsync new contacts.
            SOCIALD_LOG_INFO("the My Contacts group was found, but atom id not parsed correctly for account:" << accountId);
        } else {
            collection.setExtendedMetaData(CollectionKeyAtomId, myContactsGroupAtomId);
            SOCIALD_LOG_TRACE("found atom id" << myContactsGroupAtomId
                              << "for My Contacts group; continuing contact sync with account" << accountId);
        }

        // we can now continue with contact sync.
        m_sqliteSync[accountId]->m_collection = collection;
        m_sqliteSync[accountId]->remoteCollectionsDetermined(QList<QContactCollection>() << collection);

    } else if (!atom->nextEntriesUrl().isEmpty()) {
        // request more groups if they exist.
        startIndex += SOCIALD_GOOGLE_MAX_CONTACT_ENTRY_RESULTS;
        requestData(accountId, startIndex, atom->nextEntriesUrl(), lastSyncTimestamp, ContactGroupRequest);

    } else {
        // couldn't find the My Contacts group.
        // We don't consider this a fatal error,
        // instead we just refuse to upsync new contacts.
        SOCIALD_LOG_INFO("unable to find My Contacts group when syncing Google contacts for account:" << accountId << "; upsync disabled.");

        // we can now continue with contact sync.
        m_sqliteSync[accountId]->remoteCollectionsDetermined(QList<QContactCollection>());
    }

    delete atom;
    decrementSemaphore(accountId);
}

void GoogleTwoWayContactSyncAdaptor::contactsFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int startIndex = reply->property("startIndex").toInt();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QDateTime lastSyncTimestamp = reply->property("lastSyncTimestamp").toDateTime();
    ContactChangeNotifier contactChangeNotifier = static_cast<ContactChangeNotifier>(reply->property("contactChangeNotifier").toInt());
    bool isError = reply->property("isError").toBool();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    if (isError) {
        SOCIALD_LOG_ERROR("error occurred when performing contacts request for Google account" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    } else if (data.isEmpty()) {
        SOCIALD_LOG_ERROR("no contact data in reply from Google with account" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    GoogleContactStream parser(false, accountId);
    GoogleContactAtom *atom = parser.parse(data);

    if (!atom) {
        SOCIALD_LOG_ERROR("unable to parse contacts data from reply from Google using account with id" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    SOCIALD_LOG_TRACE("received information about" <<
                      atom->entryContacts().size() << "add/mod contacts and " <<
                      atom->deletedEntryContacts().size() << "del contacts" <<
                      "for account" << accountId);

    GoogleContactSqliteSyncAdaptor *sqliteSync = m_sqliteSync[accountId];

    // for each remote contact, there are some associated XML elements which
    // could not be stored in QContactDetail form (eg, link URIs etc).
    // build up some datastructures to help us retrieve that information
    // when we need it.
    const QList<QPair<QContact, QStringList> > remoteAddModContacts = atom->entryContacts();
    for (const QPair<QContact, QStringList> &remoteAddModContact : remoteAddModContacts) {
        QContact c = remoteAddModContact.first;
        c.setCollectionId(sqliteSync->m_collection.id());

        const QString guid = c.detail<QContactGuid>().guid();
        m_unsupportedXmlElements[accountId].insert(guid, remoteAddModContact.second);

        const QString newEtag = c.detail<QContactOriginMetadata>().id();
        const QMap<QString, QString>::iterator contactIdIter = m_contactIds[accountId].find(guid);
        if (contactIdIter == m_contactIds[accountId].end()) {
            m_remoteAdds[accountId].append(c);
        } else {
            c.setId(QContactId::fromString(contactIdIter.value()));
            m_remoteMods[accountId].append(c);
        }
        m_contactEtags[accountId].insert(guid, newEtag);
    }

    const QList<QContact> remoteDelContacts = atom->deletedEntryContacts();
    for (QContact c : remoteDelContacts) {
        const QString guid = c.detail<QContactGuid>().guid();
        c.setId(QContactId::fromString(m_contactIds[accountId].value(guid)));
        c.setCollectionId(sqliteSync->m_collection.id());
        m_contactAvatars[accountId].remove(guid); // just in case the avatar was outstanding.
        m_remoteDels[accountId].append(c);
    }

    if (!atom->nextEntriesUrl().isEmpty()) {
        // request more if they exist.
        startIndex += SOCIALD_GOOGLE_MAX_CONTACT_ENTRY_RESULTS;
        SOCIALD_LOG_TRACE("more contact sync information is available server-side; performing another request with account" << accountId);
        requestData(accountId, startIndex, atom->nextEntriesUrl(), lastSyncTimestamp, ContactRequest, contactChangeNotifier);
    } else {
        // we're finished downloading the remote changes - we should sync local changes up.
        SOCIALD_LOG_INFO("Google contact sync with account" << accountId <<
                         "got remote changes: A/M/R:"
                         << m_remoteAdds[accountId].count()
                         << m_remoteMods[accountId].count()
                         << m_remoteDels[accountId].count());

        continueSync(accountId, accessToken, contactChangeNotifier);
    }

    delete atom;
    decrementSemaphore(accountId);
}

void GoogleTwoWayContactSyncAdaptor::continueSync(int accountId, const QString &accessToken, ContactChangeNotifier contactChangeNotifier)
{
    // early out in case we lost connectivity
    if (syncAborted()) {
        SOCIALD_LOG_ERROR("aborting sync of account" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        // note: don't decrement here - it's done by contactsFinishedHandler().
        return;
    }

    // for each of the addmods, we need to fixup the contact avatars.
    transformContactAvatars(m_remoteAdds[accountId], accountId, accessToken);
    transformContactAvatars(m_remoteMods[accountId], accountId, accessToken);

    // now store the changes locally
    SOCIALD_LOG_TRACE("storing remote changes locally for account" << accountId);

    // now push those changes up to google.
    GoogleContactSqliteSyncAdaptor *sqliteSync = m_sqliteSync[accountId];
    if (contactChangeNotifier == DetermineRemoteContactChanges) {
        sqliteSync->remoteContactChangesDetermined(sqliteSync->m_collection,
                                                   m_remoteAdds[accountId],
                                                   m_remoteMods[accountId],
                                                   m_remoteDels[accountId]);
    } else {
        sqliteSync->remoteContactsDetermined(sqliteSync->m_collection, m_remoteAdds[accountId] + m_remoteMods[accountId]);
    }
}

void GoogleTwoWayContactSyncAdaptor::upsyncLocalChanges(const QDateTime &localSince,
                                                        const QList<QContact> &locallyAdded,
                                                        const QList<QContact> &locallyModified,
                                                        const QList<QContact> &locallyDeleted,
                                                        int accId)
{
    QSet<QString> alreadyEncoded; // shouldn't be necessary, as determineLocalChanges should already ensure distinct result sets.
    for (const QContact &c : locallyDeleted) {
        const QString &guid = c.detail<QContactGuid>().guid();
        m_localDels[accId].append(c);
        m_contactAvatars[accId].remove(guid); // just in case the avatar was outstanding.
        alreadyEncoded.insert(guid);
    }
    for (const QContact &c : locallyAdded) {
        const QString guid = c.detail<QContactGuid>().guid();
        if (!alreadyEncoded.contains(guid)) {
            m_localAdds[accId].append(c);
            alreadyEncoded.insert(guid);
        }
    }
    for (const QContact &c : locallyModified) {
        if (!alreadyEncoded.contains(c.detail<QContactGuid>().guid())) {
            m_localMods[accId].append(c);
        }
    }

    SOCIALD_LOG_INFO("Google account:" << accId <<
                     "upsyncing local A/M/R:" << locallyAdded.count() << "/" << locallyModified.count() << "/" << locallyDeleted.count() <<
                     "since:" << localSince.toString(Qt::ISODate));

    upsyncLocalChangesList(accId);
}

bool GoogleTwoWayContactSyncAdaptor::batchRemoteChanges(int accountId,
                                                        BatchedUpdate *batchedUpdate,
                                                        QList<QContact> *contacts, GoogleContactStream::UpdateType updateType)
{
    for (int i = contacts->size() - 1; i >= 0; --i) {
        QContact contact = contacts->takeAt(i);
        QStringList extraXmlElements = m_unsupportedXmlElements[accountId].value(contact.detail<QContactGuid>().guid());
        if (updateType == GoogleContactStream::Add) {
            // new contacts need to be inserted into the My Contacts group
            GoogleContactSqliteSyncAdaptor *sqliteSync = m_sqliteSync[accountId];
            QString myContactsGroupAtomId = collectionAtomId(sqliteSync->m_collection);
            if (myContactsGroupAtomId.isEmpty()) {
                SOCIALD_LOG_INFO("skipping upload of locally added contact" << contact.id().toString() <<
                                 "to account" << accountId << "due to unknown My Contacts group atom id");
            } else {
                extraXmlElements.append(QStringLiteral("<gContact:groupMembershipInfo deleted=\"false\" href=\"%1\"></gContact:groupMembershipInfo>").arg(myContactsGroupAtomId));
                batchedUpdate->batch.insertMulti(updateType, qMakePair(contact, extraXmlElements));
                batchedUpdate->batchCount++;
            }
        } else {
            batchedUpdate->batch.insertMulti(updateType, qMakePair(contact, extraXmlElements));
            batchedUpdate->batchCount++;
        }

        if (batchedUpdate->batchCount == SOCIALD_GOOGLE_MAX_CONTACT_ENTRY_RESULTS || i == 0) {
            GoogleContactStream encoder(false, accountId, m_emailAddresses[accountId]);
            QByteArray encodedContactUpdates = encoder.encode(batchedUpdate->batch);
            SOCIALD_LOG_TRACE("storing a batch of" << batchedUpdate->batchCount
                              << "local changes to remote server for account" << accountId);
            batchedUpdate->batch.clear();
            batchedUpdate->batchCount = 0;
            storeToRemote(accountId, m_accessTokens[accountId], encodedContactUpdates);
            return true;
        }
    }

    return false;
}

void GoogleTwoWayContactSyncAdaptor::upsyncLocalChangesList(int accountId)
{
    bool postedData = false;
    if (!m_accountSyncProfile || m_accountSyncProfile->syncDirection() != Buteo::SyncProfile::SYNC_DIRECTION_FROM_REMOTE) {
        // two-way sync is the default setting.  Upsync the changes.
        BatchedUpdate batch;
        postedData = batchRemoteChanges(accountId, &batch, &m_localMods[accountId], GoogleContactStream::Modify);
        if (!postedData) {
            postedData = batchRemoteChanges(accountId, &batch, &m_localAdds[accountId], GoogleContactStream::Add);
        }
        if (!postedData) {
            postedData = batchRemoteChanges(accountId, &batch, &m_localDels[accountId], GoogleContactStream::Remove);
        }
    } else {
        SOCIALD_LOG_INFO("skipping upload of local contacts changes due to profile direction setting for account" << accountId);
    }

    if (!postedData) {
        // nothing left to upsync.  attempt to download any outstanding avatars.
        queueOutstandingAvatars(accountId, m_accessTokens[accountId]);

        // notify TWCSA that the upsync is complete.
        m_sqliteSync[accountId]->localChangesStoredRemotely(m_sqliteSync[accountId]->m_collection,
                                                            m_localAdds[accountId],
                                                            m_localMods[accountId]);
    }
}

void GoogleTwoWayContactSyncAdaptor::storeToRemote(int accountId, const QString &accessToken, const QByteArray &encodedContactUpdates)
{
    QUrl requestUrl(QUrl(QString(QLatin1String("https://www.google.com/m8/feeds/contacts/default/full/batch"))));
    QNetworkRequest req(requestUrl);
    req.setRawHeader("GData-Version", "3.0");
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ") + accessToken).toUtf8());
    req.setRawHeader(QString(QLatin1String("Content-Type")).toUtf8(),
                     QString(QLatin1String("application/atom+xml; charset=UTF-8; type=feed")).toUtf8());
    req.setHeader(QNetworkRequest::ContentLengthHeader, encodedContactUpdates.size());
    req.setRawHeader(QString(QLatin1String("If-Match")).toUtf8(),
                     QString(QLatin1String("*")).toUtf8());

    // we're posting data.  Increment the semaphore so that we know we're still busy.
    incrementSemaphore(accountId);
    QNetworkReply *reply = m_networkAccessManager->post(req, encodedContactUpdates);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        connect(reply, &QNetworkReply::finished,
                this, &GoogleTwoWayContactSyncAdaptor::postFinishedHandler);
        connect(reply, static_cast<void (QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error),
                this, &GoogleTwoWayContactSyncAdaptor::postErrorHandler);
        connect(reply, &QNetworkReply::sslErrors,
                this, &GoogleTwoWayContactSyncAdaptor::postErrorHandler);
        m_apiRequestsRemaining[accountId] = m_apiRequestsRemaining[accountId] - 1;
        setupReplyTimeout(accountId, reply);
    } else {
        SOCIALD_LOG_ERROR("unable to post contacts to Google account with id" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
    }
}

void GoogleTwoWayContactSyncAdaptor::postFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray response = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    if (reply->property("isError").toBool()) {
        SOCIALD_LOG_ERROR("error occurred posting contact data to google with account" << accountId << "," <<
                          "got response:" << QString::fromUtf8(response));
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    GoogleContactStream parser(false, accountId);
    GoogleContactAtom *atom = parser.parse(response);
    QMap<QString, GoogleContactAtom::BatchOperationResponse> operationResponses = atom->batchOperationResponses();

    bool errorOccurredInBatch = false;
    foreach (const GoogleContactAtom::BatchOperationResponse &response, operationResponses) {
        if (response.isError) {
            errorOccurredInBatch = true;
            SOCIALD_LOG_DEBUG("batch operation error:\n"
                              "    id:     " << response.operationId << "\n"
                              "    type:   " << response.type << "\n"
                              "    code:   " << response.code << "\n"
                              "    reason: " << response.reason << "\n"
                              "    descr:  " << response.reasonDescription << "\n");
        }
    }

    if (errorOccurredInBatch) {
        SOCIALD_LOG_ERROR("error occurred during batch operation with Google account" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    // continue with more, if there were more than one page of updates to post.
    upsyncLocalChangesList(accountId);

    // finished with this request, so decrementing semaphore.
    decrementSemaphore(accountId);
}

void GoogleTwoWayContactSyncAdaptor::postErrorHandler()
{
    sender()->setProperty("isError", QVariant::fromValue<bool>(true));
}

void GoogleTwoWayContactSyncAdaptor::queueOutstandingAvatars(int accountId, const QString &accessToken)
{
    int queuedCount = 0;
    for (QMap<QString, QString>::const_iterator it = m_contactAvatars[accountId].constBegin();
            it != m_contactAvatars[accountId].constEnd(); ++it) {
        if (!it.value().isEmpty() && queueAvatarForDownload(accountId, accessToken, it.key(), it.value())) {
            queuedCount++;
        }
    }

    SOCIALD_LOG_DEBUG("queued" << queuedCount << "avatars for download for account" << accountId);
}

bool GoogleTwoWayContactSyncAdaptor::queueAvatarForDownload(int accountId, const QString &accessToken, const QString &contactGuid, const QString &imageUrl)
{
    if (m_apiRequestsRemaining[accountId] > 0 && !m_queuedAvatarsForDownload[accountId].contains(contactGuid)) {
        m_apiRequestsRemaining[accountId] = m_apiRequestsRemaining[accountId] - 1;
        m_queuedAvatarsForDownload[accountId][contactGuid] = imageUrl;

        QVariantMap metadata;
        metadata.insert(IMAGE_DOWNLOADER_ACCOUNT_ID_KEY, accountId);
        metadata.insert(IMAGE_DOWNLOADER_TOKEN_KEY, accessToken);
        metadata.insert(IMAGE_DOWNLOADER_IDENTIFIER_KEY, contactGuid);
        incrementSemaphore(accountId);
        m_workerObject->queue(imageUrl, metadata);

        return true;
    }

    return false;
}

void GoogleTwoWayContactSyncAdaptor::transformContactAvatars(QList<QContact> &remoteContacts, int accountId, const QString &accessToken)
{
    // The avatar detail from the remote contact will be of the form:
    // https://www.google.com/m8/feeds/photos/media/user@gmail.com/userId
    // We need to:
    // 1) transform this to a local filename.
    // 2) determine if the local file exists.
    // 3) if not, trigger downloading the avatar.

    for (int i = 0; i < remoteContacts.size(); ++i) {
        QContact &curr(remoteContacts[i]);

        // We only deal with the first avatar from the contact.  If it has multiple,
        // then later avatars will not be transformed.  TODO: fix this.
        // We also only bother to do this for contacts with a GUID, as we don't
        // store locally any contact without one.
        QString contactGuid = curr.detail<QContactGuid>().guid();
        if (curr.details<QContactAvatar>().size() && !contactGuid.isEmpty()) {
            // we have a remote avatar which we need to transform.
            QContactAvatar avatar = curr.detail<QContactAvatar>();
            Q_FOREACH (const QContactAvatar &av, curr.details<QContactAvatar>()) {
                if (av.value(QContactAvatar__FieldAvatarMetadata).toString() == QStringLiteral("picture")) {
                    avatar = av;
                    break;
                }
            }
            QString remoteImageUrl = avatar.imageUrl().toString();
            if (!remoteImageUrl.isEmpty() && !avatar.imageUrl().isLocalFile()) {
                // transform to a local file name.
                QString localFileName = GoogleContactImageDownloader::staticOutputFile(
                        contactGuid, remoteImageUrl);

                // and trigger downloading the image, if it doesn't already exist.
                // this means that we shouldn't download images needlessly after
                // first sync, but it also means that if it updates/changes on the
                // server side, we also won't retrieve any updated image.
                if (QFile::exists(localFileName)) {
                    QImageReader reader(localFileName);
                    if (reader.canRead()) {
                        // avatar image already exists, update the detail in the contact.
                        avatar.setImageUrl(localFileName);
                        curr.saveDetail(&avatar);
                    } else {
                        // not a valid image file.  Could be artifact from an error.
                        QFile::remove(localFileName);
                    }
                }

                if (!QFile::exists(localFileName)) {
                    // temporarily remove the avatar from the contact
                    m_contactAvatars[accountId].insert(contactGuid, remoteImageUrl);
                    curr.removeDetail(&avatar);
                    // then trigger the download
                    queueAvatarForDownload(accountId, accessToken, contactGuid, remoteImageUrl);
                }
            }
        }
    }
}

void GoogleTwoWayContactSyncAdaptor::imageDownloaded(const QString &url, const QString &path,
                                                     const QVariantMap &metadata)
{
    // Load finished, update the avatar, decrement semaphore
    int accountId = metadata.value(IMAGE_DOWNLOADER_ACCOUNT_ID_KEY).toInt();
    QString contactGuid = metadata.value(IMAGE_DOWNLOADER_IDENTIFIER_KEY).toString();

    // Empty path signifies that an error occurred.
    if (path.isEmpty()) {
        SOCIALD_LOG_ERROR("Unable to download avatar" << url);
    } else {
        // no longer outstanding.
        m_contactAvatars[accountId].remove(contactGuid);
        m_queuedAvatarsForDownload[accountId].remove(contactGuid);
        m_downloadedContactAvatars[accountId].insert(contactGuid, path);
    }

    decrementSemaphore(accountId);
}

void GoogleTwoWayContactSyncAdaptor::purgeAccount(int pid)
{
    QContactCollectionId collectionId = findCollection(*m_contactManager, MyContactsCollectionName, pid).id();
    if (collectionId.isNull()) {
        SOCIALD_LOG_ERROR("Nothing to purge, no collection has been saved for account" << pid);
        return;
    }

    if (m_contactManager->removeCollection(collectionId)) {
        SOCIALD_LOG_INFO("purged account" << pid << "and successfully removed collection" << collectionId);
    } else {
        SOCIALD_LOG_ERROR("Failed to remove My Contacts collection during purge of account" << pid
                          << ":" << m_contactManager->error());
    }
}

void GoogleTwoWayContactSyncAdaptor::finalize(int accountId)
{
    if (m_accessTokens[accountId].isEmpty()
            || syncAborted()
            || status() == SocialNetworkSyncAdaptor::Error) {
        // account failure occurred before sync process was started,
        // or other error occurred during sync.
        // in this case we have nothing left to do except cleanup.
        return;
    }

    // sync was successful, allow cleaning up contacts from removed accounts.
    m_allowFinalCleanup = true;

    // first, ensure we update any avatars required.
    if (m_downloadedContactAvatars[accountId].size()) {
        // load all contacts from the database.  We need all details, to avoid clobber.
        QContactCollectionFilter collectionFilter;
        collectionFilter.setCollectionId(m_sqliteSync[accountId]->m_collection.id());
        QList<QContact> savedContacts = m_contactManager->contacts(collectionFilter);

        // find the contacts we need to update.
        QMap<QString, QContact> contactsToSave;
        for (auto it = m_downloadedContactAvatars[accountId].constBegin();
                it != m_downloadedContactAvatars[accountId].constEnd(); ++it) {
            QContact c = findContact(savedContacts, it.key());
            if (c.isEmpty()) {
                SOCIALD_LOG_ERROR("Not saving avatar, cannot find contact with guid" << it.key());
            } else {
                // we have downloaded the avatar for this contact, and need to update it.
                QContactAvatar a;
                Q_FOREACH (const QContactAvatar &av, c.details<QContactAvatar>()) {
                    if (av.value(QContactAvatar__FieldAvatarMetadata).toString() == QStringLiteral("picture")) {
                        a = av;
                        break;
                    }
                }
                a.setValue(QContactAvatar__FieldAvatarMetadata, QVariant::fromValue<QString>(QStringLiteral("picture")));
                a.setImageUrl(it.value());
                if (c.saveDetail(&a)) {
                    contactsToSave[c.detail<QContactGuid>().guid()] = c;
                } else {
                    SOCIALD_LOG_ERROR("Unable to save avatar" << it.value()
                                      << "to contact" << c.detail<QContactGuid>().guid());
                }
            }
        }

        QList<QContact> saveList = contactsToSave.values();
        if (m_contactManager->saveContacts(&saveList)) {
            SOCIALD_LOG_INFO("finalize: added avatars for" << saveList.size() << "contacts from account" << accountId);
        } else {
            SOCIALD_LOG_ERROR("finalize: error adding avatars for" << saveList.size() <<
                              "Google contacts from account" << accountId);
        }
    }
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
    // Also, determine if any avatars were not synced, and remove those details.

    // first, get a list of all existing google account ids
    QList<int> googleAccountIds;
    QList<int> purgeAccountIds;
    QList<int> currentAccountIds;
    QList<uint> uaids = m_accountManager->accountList();
    foreach (uint uaid, uaids) {
        currentAccountIds.append(static_cast<int>(uaid));
    }
    foreach (int currId, currentAccountIds) {
        Accounts::Account *act = Accounts::Account::fromId(m_accountManager, currId, this);
        if (act) {
            if (act->providerName() == QString(QLatin1String("google"))) {
                // this account still exists, no need to purge its content.
                googleAccountIds.append(currId);
            }
            act->deleteLater();
        }
    }

    // second, get all contacts which have been synced from Google.
    QList<QContact> googleContacts;
    for (auto it = m_sqliteSync.begin(); it != m_sqliteSync.end(); ++it) {
        QContactCollectionFilter collectionFilter;
        collectionFilter.setCollectionId(it.value()->m_collection.id());
        QContactFetchHint noRelationships;
        noRelationships.setOptimizationHints(QContactFetchHint::NoRelationships);
        noRelationships.setDetailTypesHint(QList<QContactDetail::DetailType>() << QContactGuid::Type << QContactAvatar::Type);
        googleContacts.append(m_contactManager->contacts(collectionFilter, QList<QContactSortOrder>(), noRelationships));
    }

    // third, find all account ids from which contacts have been synced
    const QList<QContactCollection> collections = m_contactManager->collections();
    for (const QContactCollection &collection : collections) {
        if (collection.metaData(QContactCollection::KeyName).toString() == MyContactsCollectionName) {
            const int purgeId = collection.extendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_ACCOUNTID).toInt();
            if (purgeId && !googleAccountIds.contains(purgeId) && !purgeAccountIds.contains(purgeId)) {
                // this account no longer exists, and needs to be purged.
                purgeAccountIds.append(purgeId);
            }
        }
    }

    // fourth, remove any non-existent avatar details.
    // We save these first, in case some contacts get removed by purge.
    // Note that this entire codeblock is only required temporarily,
    // as the code from previous versions could leave "erroneous" avatars
    // on disk / in database, but the current code will not.
    // We should remove this block (and the subsequent save operation)
    // in a future update.
    QMap<QString, QContact> contactsToSave;
    for (int i = 0; i < googleContacts.size(); ++i) {
        QContact contact = googleContacts.at(i);

        const QString contactGuid(contact.detail<QContactGuid>().guid());
        QStringList guidParts = contactGuid.split(":");
        QString accountIdStr = guidParts.size() ? guidParts.first() : QString();
        if (!accountIdStr.isEmpty()) {
            int accountId = accountIdStr.toInt();

            // remove any nonexistent/error avatar details from this contact.
            QList<QContactAvatar> allAvatars = contact.details<QContactAvatar>();
            for (int j = 0; j < allAvatars.size(); ++j) {
                QContactAvatar av = allAvatars[j];
                if (!av.imageUrl().isEmpty()) {
                    // this avatar may have failed to sync.
                    QUrl avatarUrl = av.imageUrl();
                    QString avatarPath = av.imageUrl().toString();
                    if (avatarUrl.isLocalFile()) {
                        if (QFile::exists(avatarPath)) {
                            QImageReader reader(avatarPath);
                            if (!reader.canRead()) {
                                // remove artifacts of previous (failed) syncs if necessary.
                                QFile::remove(avatarPath);
                            }
                        }
                        if (!QFile::exists(avatarPath)) {
                            // we should remove this nonexistent avatar from this contact.
                            if (accountId != 0 && !purgeAccountIds.contains(accountId)) {
                                // download failed, remove it from the contact.
                                contact.removeDetail(&av);
                                contactsToSave[contactGuid] = contact;
                            }
                        }
                    }
                }
            }
        }
    }

    QList<QContact> saveList = contactsToSave.values();
    QList<QContactDetail::DetailType> typeMask; typeMask << QContactDetail::TypeAvatar;
    if (m_contactManager->saveContacts(&saveList, typeMask)) {
        SOCIALD_LOG_INFO("finalCleanup() fixed up avatars from" << saveList.size() << "Google contacts");
    } else {
        SOCIALD_LOG_ERROR("finalCleanup() failed to save non-existent avatar removals for Google contacts");
    }

    // fifth, purge all data for those account ids which no longer exist.
    if (purgeAccountIds.size()) {
        SOCIALD_LOG_INFO("finalCleanup() purging contacts from" << purgeAccountIds.size() << "non-existent Google accounts");
        foreach (int purgeId, purgeAccountIds) {
            purgeAccount(purgeId);
        }
    }
}

void GoogleTwoWayContactSyncAdaptor::readSyncStateData(const QContactCollection &collection)
{
    const int accountId = collection.extendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_ACCOUNTID).toInt();

    QContactCollectionFilter collectionFilter;
    collectionFilter.setCollectionId(collection.id());
    QContactFetchHint noRelationships;
    noRelationships.setOptimizationHints(QContactFetchHint::NoRelationships);
    QList<QContact> savedContacts = m_contactManager->contacts(collectionFilter, QList<QContactSortOrder>(), noRelationships);

    // m_unsupportedElements
    QVariant ueValue = collection.extendedMetaData(QStringLiteral("unsupportedElements"));
    QByteArray ueValueBA = ueValue.toByteArray();
    QJsonObject ueJsonObj = QJsonDocument::fromBinaryData(ueValueBA).object();
    QStringList contactGuids = ueJsonObj.keys();
    QMap<QString, QStringList> guidToUnsupportedElements;
    foreach (const QString &guid, contactGuids) {
        QVariantList unsupportedElementsVL = ueJsonObj.value(guid).toArray().toVariantList();
        QStringList unsupportedElements;
        foreach (const QVariant &v, unsupportedElementsVL) {
            if (!v.toString().isEmpty()) {
                unsupportedElements.append(v.toString());
            }
        }

        guidToUnsupportedElements.insert(guid, unsupportedElements);
    }
    m_unsupportedXmlElements[accountId] = guidToUnsupportedElements;

    m_contactEtags[accountId].clear();
    m_contactIds[accountId].clear();
    for (const QContact &contact : savedContacts) {
        const QString contactGuid = contact.detail<QContactGuid>().guid();
        if (contactGuid.isEmpty()) {
            SOCIALD_LOG_ERROR("No guid found for saved contact:" << contact.id());
            continue;
        }

        // m_contactEtags
        const QString etag = contact.detail<QContactOriginMetadata>().id();
        if (!etag.isEmpty()) {
            m_contactEtags[accountId][contactGuid] = etag;
        }

        // m_contactIds
        m_contactIds[accountId][contactGuid] = contact.id().toString();
    }
}

void GoogleTwoWayContactSyncAdaptor::storeSyncStateData(QContactCollection *collection)
{
    const int accountId = collection->extendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_ACCOUNTID).toInt();

    collection->setExtendedMetaData(CollectionKeyLastSync, QDateTime::currentDateTimeUtc());

    // m_unsupportedXmlElements
    QJsonObject ueJsonObj;
    for (QMap<QString, QStringList>::const_iterator it = m_unsupportedXmlElements[accountId].constBegin();
            it != m_unsupportedXmlElements[accountId].constEnd(); ++it) {
        ueJsonObj.insert(it.key(), QJsonValue(QJsonArray::fromStringList(it.value())));
    }
    QJsonDocument ueJsonDoc(ueJsonObj);
    QVariant ueValue(ueJsonDoc.toBinaryData());
    collection->setExtendedMetaData("unsupportedElements", ueValue);
}
