/****************************************************************************
 **
 ** Copyright (c) 2014 - 2019 Jolla Ltd.
 ** Copyright (c) 2020 Open Mobile Platform LLC.
 **
 ****************************************************************************/

#include "vkcontactsyncadaptor.h"
#include "vkcontactimagedownloader.h"

#include "constants_p.h"
#include "trace.h"

#include <twowaycontactsyncadaptor_impl.h>
#include <qtcontacts-extensions_manager_impl.h>
#include <qcontactstatusflags_impl.h>
#include <contactmanagerengine.h>

#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtGui/QImageReader>

#include <QtContacts/QContact>
#include <QtContacts/QContactCollection>
#include <QtContacts/QContactCollectionFilter>
#include <QtContacts/QContactGuid>
#include <QtContacts/QContactName>
#include <QtContacts/QContactNickname>
#include <QtContacts/QContactAvatar>
#include <QtContacts/QContactAddress>
#include <QtContacts/QContactUrl>
#include <QtContacts/QContactGender>
#include <QtContacts/QContactBirthday>
#include <QtContacts/QContactPhoneNumber>

//libaccounts-qt5
#include <Accounts/Account>
#include <Accounts/Manager>

#define SOCIALD_VK_MAX_CONTACT_ENTRY_RESULTS 200

static const char *IMAGE_DOWNLOADER_TOKEN_KEY = "token";
static const char *IMAGE_DOWNLOADER_ACCOUNT_ID_KEY = "account_id";
static const char *IMAGE_DOWNLOADER_IDENTIFIER_KEY = "identifier";

namespace {

const QString FriendCollectionName = QStringLiteral("VK");

bool saveNonexportableDetail(QContact &c, QContactDetail &d)
{
    d.setValue(QContactDetail__FieldNonexportable, QVariant::fromValue<bool>(true));
    return c.saveDetail(&d, QContact::IgnoreAccessConstraints);
}

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

QList<QContactCollection> findAllCollections(QContactManager &contactManager, int accountId)
{
    QtContactsSqliteExtensions::ContactManagerEngine *cme = QtContactsSqliteExtensions::contactManagerEngine(contactManager);
    QContactManager::Error error = QContactManager::NoError;

    QList<QContactCollection> addedCollections;
    QList<QContactCollection> modifiedCollections;
    QList<QContactCollection> deletedCollections;
    QList<QContactCollection> unmodifiedCollections;

    if (!cme->fetchCollectionChanges(accountId,
                                     qAppName(),
                                     &addedCollections,
                                     &modifiedCollections,
                                     &deletedCollections,
                                     &unmodifiedCollections,
                                     &error)) {
        qCWarning(lcSocialPlugin) << "Cannot find collections for account" << accountId
                            << "app" << qAppName() << "error:" << error;
        return QList<QContactCollection>();
    }

    return addedCollections + modifiedCollections + deletedCollections + unmodifiedCollections;
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

}

//---------------

VKContactSqliteSyncAdaptor::VKContactSqliteSyncAdaptor(int accountId, VKContactSyncAdaptor *parent)
    : QtContactsSqliteExtensions::TwoWayContactSyncAdaptor(accountId, qAppName(), *parent->m_contactManager)
    , q(parent)
    , m_accountId(accountId)
{
    m_collection = findCollection(contactManager(), FriendCollectionName, m_accountId);
    if (m_collection.id().isNull()) {
        qCDebug(lcSocialPlugin) << "No friends collection saved yet for account:" << m_accountId;

        m_collection.setMetaData(QContactCollection::KeyName, FriendCollectionName);
        m_collection.setMetaData(QContactCollection::KeyDescription, QStringLiteral("VK friend contacts"));
        m_collection.setMetaData(QContactCollection::KeyColor, QStringLiteral("steelblue"));
        m_collection.setMetaData(QContactCollection::KeySecondaryColor, QStringLiteral("lightsteelblue"));
        m_collection.setExtendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_APPLICATIONNAME, QCoreApplication::applicationName());
        m_collection.setExtendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_ACCOUNTID, m_accountId);
        m_collection.setExtendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_READONLY, true);
    } else {
        qCDebug(lcSocialPlugin) << "Found friends collection" << m_collection.id() << "for account:" << m_accountId;
    }
}

VKContactSqliteSyncAdaptor::~VKContactSqliteSyncAdaptor()
{
}

bool VKContactSqliteSyncAdaptor::determineRemoteCollections()
{
    remoteCollectionsDetermined(QList<QContactCollection>() << m_collection);
    return true;
}

bool VKContactSqliteSyncAdaptor::deleteRemoteCollection(const QContactCollection &collection)
{
    qCWarning(lcSocialPlugin) << "Upsync to remote not supported, not deleting collection" << collection.id();
    return true;
}

bool VKContactSqliteSyncAdaptor::determineRemoteContacts(const QContactCollection &collection)
{
    Q_UNUSED(collection)

    q->requestData(accountIdForCollection(collection), 0);
    return true;
}

bool VKContactSqliteSyncAdaptor::storeLocalChangesRemotely(const QContactCollection &collection,
                                                           const QList<QContact> &addedContacts,
                                                           const QList<QContact> &modifiedContacts,
                                                           const QList<QContact> &deletedContacts)
{
    Q_UNUSED(collection)
    Q_UNUSED(addedContacts)
    Q_UNUSED(modifiedContacts)

    for (const QContact &contact : deletedContacts) {
        q->deleteDownloadedAvatar(contact);
    }

    qCDebug(lcSocialPlugin) << "Upsync to remote not supported, ignoring remote changes for"
                      << collection.id();
    return true;
}

void VKContactSqliteSyncAdaptor::storeRemoteChangesLocally(const QContactCollection &collection,
                                                           const QList<QContact> &addedContacts,
                                                           const QList<QContact> &modifiedContacts,
                                                           const QList<QContact> &deletedContacts)
{
    Q_UNUSED(addedContacts)
    Q_UNUSED(modifiedContacts)

    for (const QContact &contact : deletedContacts) {
        q->deleteDownloadedAvatar(contact);
    }

    QtContactsSqliteExtensions::TwoWayContactSyncAdaptor::storeRemoteChangesLocally(collection, addedContacts, modifiedContacts, deletedContacts);
}

void VKContactSqliteSyncAdaptor::syncFinishedSuccessfully()
{
    qCDebug(lcSocialPlugin) << "Sync finished OK";

    // If this is the first sync, TWCSA will have saved the collection and given it a valid id, so
    // update m_collection so that any post-sync operations (e.g. saving of queued avatar downloads)
    // will refer to a valid collection.
    const QContactCollection savedCollection = findCollection(contactManager(), FriendCollectionName, m_accountId);
    if (savedCollection.id().isNull()) {
        qCDebug(lcSocialPlugin) << "Error: cannot find saved friends collection!";
    } else {
        m_collection.setId(savedCollection.id());
    }
}

void VKContactSqliteSyncAdaptor::syncFinishedWithError()
{
    qCDebug(lcSocialPlugin) << "Sync finished with error";
}

int VKContactSqliteSyncAdaptor::accountIdForCollection(const QContactCollection &collection)
{
    return collection.extendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_ACCOUNTID).toInt();
}

//----------------------------------

VKContactSyncAdaptor::VKContactSyncAdaptor(QObject *parent)
    : VKDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::Contacts, parent)
    , m_contactManager(new QContactManager(QStringLiteral("org.nemomobile.contacts.sqlite")))
    , m_workerObject(new VKContactImageDownloader())
{
    connect(m_workerObject, &AbstractImageDownloader::imageDownloaded,
            this, &VKContactSyncAdaptor::imageDownloaded);

    // can sync, enabled
    setInitialActive(true);
}

VKContactSyncAdaptor::~VKContactSyncAdaptor()
{
    delete m_workerObject;
}

QString VKContactSyncAdaptor::syncServiceName() const
{
    return QStringLiteral("vk-contacts");
}

void VKContactSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    m_apiRequestsRemaining[accountId] = 99; // assume we can make up to 99 requests per sync, before being throttled.

    // call superclass impl.
    VKDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void VKContactSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode)
{
    const QList<QContactCollection> collections = findAllCollections(*m_contactManager, oldId);
    if (collections.isEmpty()) {
        qCWarning(lcSocialPlugin) << "Nothing to purge, no collection has been saved for account" << oldId;
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
            const QContactAvatar avatar = contact.detail<QContactAvatar>();
            const QString imageUrl = avatar.imageUrl().toString();
            if (!imageUrl.isEmpty()) {
                if (!QFile::remove(imageUrl)) {
                    qCWarning(lcSocialPlugin) << "Failed to remove avatar:" << imageUrl;
                }
            }
        }
    }

    QList<QContactCollectionId> collectionIds;
    for (const QContactCollection &collection : collections) {
        collectionIds.append(collection.id());
    }

    // Delete the collections and their contacts.
    QtContactsSqliteExtensions::ContactManagerEngine *cme = QtContactsSqliteExtensions::contactManagerEngine(*m_contactManager);
    QContactManager::Error error = QContactManager::NoError;
    if (cme->storeChanges(nullptr,
                          nullptr,
                          collectionIds,
                          QtContactsSqliteExtensions::ContactManagerEngine::PreserveLocalChanges,
                          true,
                          &error)) {
        qCInfo(lcSocialPlugin) << "purged account" << oldId << "and successfully removed collections";
    } else {
        qCWarning(lcSocialPlugin) << "Failed to remove collection during purge of account" << oldId
                          << "error:" << error;
    }
}

void VKContactSyncAdaptor::retryThrottledRequest(const QString &request, const QVariantList &args, bool retryLimitReached)
{
    int accountId = args[0].toInt();
    if (retryLimitReached) {
        qCWarning(lcSocialPlugin) << "hit request retry limit! unable to request data from VK account with id" << accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
    } else {
        qCDebug(lcSocialPlugin) << "retrying Contacts" << request << "request for VK account:" << accountId;
        requestData(accountId, args[1].toInt());
    }
    decrementSemaphore(accountId); // finished waiting for the request.
}

void VKContactSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    // clear our cache lists if necessary.
    m_remoteContacts[accountId].clear();
    m_accessTokens[accountId] = accessToken;

    VKContactSqliteSyncAdaptor *sqliteSync = m_sqliteSync.value(accountId);
    if (sqliteSync) {
        delete sqliteSync;
    }
    sqliteSync = new VKContactSqliteSyncAdaptor(accountId, this);
    if (!sqliteSync->startSync()) {
        sqliteSync->deleteLater();
        qCWarning(lcSocialPlugin) << "unable to init sync adapter - aborting sync VK contacts with account:" << accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    m_sqliteSync[accountId] = sqliteSync;
}

void VKContactSyncAdaptor::requestData(int accountId, int startIndex)
{
    const QString accessToken = m_accessTokens[accountId];

    QUrl requestUrl;
    QUrlQuery urlQuery;
    requestUrl = QUrl(QStringLiteral("https://api.vk.com/method/friends.get"));
    if (startIndex >= 1) {
        urlQuery.addQueryItem ("offset", QString::number(startIndex));
    }
    urlQuery.addQueryItem("count", QString::number(SOCIALD_VK_MAX_CONTACT_ENTRY_RESULTS));
    urlQuery.addQueryItem("fields", QStringLiteral("uid,first_name,last_name,sex,screen_name,bdate,photo_max,contacts,city,country"));
    urlQuery.addQueryItem("access_token", accessToken);
    urlQuery.addQueryItem("v", QStringLiteral("5.21")); // version
    requestUrl.setQuery(urlQuery);

    QNetworkRequest req(requestUrl);

    // we're requesting data.  Increment the semaphore so that we know we're still busy.
    incrementSemaphore(accountId);
    QNetworkReply *reply = m_networkAccessManager->get(req);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("startIndex", startIndex);
        connect(reply, &QNetworkReply::finished,
                this, &VKContactSyncAdaptor::contactsFinishedHandler);
        connect(reply, static_cast<void (QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error),
                this, &VKContactSyncAdaptor::errorHandler);
        connect(reply, &QNetworkReply::sslErrors,
                this, &VKContactSyncAdaptor::sslErrorsHandler);
        m_apiRequestsRemaining[accountId] = m_apiRequestsRemaining[accountId] - 1;
        setupReplyTimeout(accountId, reply);
    } else {
        // request was throttled by VKNetworkAccessManager
        QVariantList args;
        args << accountId << startIndex;
        enqueueThrottledRequest(QStringLiteral("requestData"), args);

        // we are waiting to request data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId); // decremented in retryThrottledRequest().
    }
}

void VKContactSyncAdaptor::contactsFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    int startIndex = reply->property("startIndex").toInt();
    QDateTime lastSyncTimestamp = reply->property("lastSyncTimestamp").toDateTime();
    bool isError = reply->property("isError").toBool();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    qCDebug(lcSocialPluginTrace) << "received VK friends data for account:" << accountId << ":";
    Q_FOREACH (const QString &line, QString::fromUtf8(data).split('\n', QString::SkipEmptyParts)) {
        qCDebug(lcSocialPluginTrace) << line;
    }

    if (isError) {
        QVariantList args;
        args << accountId << accessToken << startIndex << lastSyncTimestamp;
        bool ok = true;
        QJsonObject parsed = parseJsonObjectReplyData(data, &ok);
        if (enqueueServerThrottledRequestIfRequired(parsed, QStringLiteral("requestData"), args)) {
            // we hit the throttle limit, let throttle timer repeat the request
            // don't decrement semaphore yet as we're still waiting for it.
            // it will be decremented in retryThrottledRequest().
            return;
        }
        qCWarning(lcSocialPlugin) << "error occurred when performing contacts request for VK account:" << accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    } else if (data.isEmpty()) {
        qCWarning(lcSocialPlugin) << "no contact data in reply from VK with account:" << accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    // parse the remote contact information from the response
    QJsonObject obj = QJsonDocument::fromJson(data).object();
    QJsonObject response = obj.value("response").toObject();
    m_remoteContacts[accountId].append(parseContacts(response.value("items").toArray(), accountId, accessToken));

    int totalCount = response.value("count").toInt();
    int seenCount = startIndex + SOCIALD_VK_MAX_CONTACT_ENTRY_RESULTS;
    if (syncAborted()) {
        qCInfo(lcSocialPlugin) << "sync aborted, not continuing sync of contacts from VK with account:" << accountId;
    } else if (totalCount > seenCount) {
        qCDebug(lcSocialPluginTrace) << "Have received" << seenCount << "contacts, now requesting:" << (seenCount+1) << "through to" << (seenCount+1+SOCIALD_VK_MAX_CONTACT_ENTRY_RESULTS);
        startIndex = seenCount;
        requestData(accountId, startIndex);
    } else {
        // We've finished downloading the remote changes
        VKContactSqliteSyncAdaptor *sqliteSync = m_sqliteSync[accountId];
        sqliteSync->remoteContactsDetermined(sqliteSync->m_collection, m_remoteContacts[accountId]);
    }

    decrementSemaphore(accountId);
}

bool VKContactSyncAdaptor::queueAvatarForDownload(int accountId, const QString &accessToken, const QString &contactGuid, const QString &imageUrl)
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

QList<QContact> VKContactSyncAdaptor::parseContacts(const QJsonArray &json, int accountId, const QString &accessToken)
{
    QList<QContact> retn;
    QJsonArray::const_iterator it = json.constBegin();

    for ( ; it != json.constEnd(); ++it) {
        const QJsonObject &obj((*it).toObject());
        if (obj.isEmpty()) continue;

        QString mobilePhone = obj.value("mobile_phone").toString();
        QString homePhone = obj.value("home_phone").toString();

        // build the contact.
        QContact c;

        QContactName name;
        name.setFirstName(obj.value("first_name").toString());
        name.setLastName(obj.value("last_name").toString());
        saveNonexportableDetail(c, name);

        QContactGuid guid;
        int idint = static_cast<int>(obj.value("id").toDouble()); // horrible hack.
        int uidint = static_cast<int>(obj.value("uid").toDouble()); // horrible hack.
        if (idint > 0) {
            guid.setGuid(QStringLiteral("%1:%2").arg(accountId).arg(QString::number(idint)));
        } else if (uidint > 0) {
            guid.setGuid(QStringLiteral("%1:%2").arg(accountId).arg(QString::number(uidint)));
        } else {
            qCWarning(lcSocialPlugin) << "unable to parse id from VK friend, skipping:" << name;
            continue;
        }
        saveNonexportableDetail(c, guid);

        if (obj.value("sex").toDouble() > 0) {
            double genderVal = obj.value("sex").toDouble();
            QContactGender gender;
            if (genderVal == 1.0) {
                gender.setGender(QContactGender::GenderFemale);
            } else {
                gender.setGender(QContactGender::GenderMale);
            }
            saveNonexportableDetail(c, gender);
        }

        if (!obj.value("bdate").toString().isEmpty() && obj.value("bdate").toString().length() > 5) {
            // DD.MM.YYYY form, we ignore DD.MM (yearless) form response.
            QContactBirthday birthday;
            birthday.setDateTime(QLocale::c().toDateTime(obj.value("bdate").toString(), "dd.MM.yyyy"));
            saveNonexportableDetail(c, birthday);
        }

        if (!obj.value("screen_name").toString().isEmpty() &&
                obj.value("screen_name").toString() != QStringLiteral("id%1").arg(c.detail<QContactGuid>().guid())) {
            QContactNickname nickname;
            nickname.setNickname(obj.value("screen_name").toString());
            saveNonexportableDetail(c, nickname);
        }

        if (!obj.value("photo_max").toString().isEmpty()) {
            QContactAvatar avatar;
            avatar.setImageUrl(QUrl(obj.value("photo_max").toString()));
            avatar.setValue(QContactAvatar::FieldMetaData, QStringLiteral("picture"));
            saveNonexportableDetail(c, avatar);
        }

        if ((!obj.value("city").toObject().isEmpty() && !obj.value("city").toObject().value("title").toString().isEmpty())
                || (!obj.value("country").toObject().isEmpty() && !obj.value("country").toObject().value("title").toString().isEmpty())) {
            QContactAddress addr;
            addr.setLocality(obj.value("city").toObject().value("title").toString());
            addr.setCountry(obj.value("country").toObject().value("title").toString());
            saveNonexportableDetail(c, addr);
        }

        if (!mobilePhone.isEmpty()) {
            QContactPhoneNumber num;
            num.setSubTypes(QList<int>() << QContactPhoneNumber::SubTypeMobile);
            num.setNumber(obj.value("mobile_phone").toString());
            saveNonexportableDetail(c, num);
        }

        if (!homePhone.isEmpty()) {
            QContactPhoneNumber num;
            num.setContexts(QContactDetail::ContextHome);
            num.setSubTypes(QList<int>() << QContactPhoneNumber::SubTypeLandline);
            num.setNumber(obj.value("mobile_phone").toString());
            saveNonexportableDetail(c, num);
        }

        QContactUrl url;
        if (idint > 0) {
            url.setUrl(QUrl(QStringLiteral("https://m.vk.com/id%1").arg(idint)));
        } else if (uidint > 0) {
            url.setUrl(QUrl(QStringLiteral("https://m.vk.com/id%1").arg(uidint)));
        }
        url.setSubType(QContactUrl::SubTypeHomePage);
        saveNonexportableDetail(c, url);

        retn.append(c);
    }

    // fixup the contact avatars.
    transformContactAvatars(retn, accountId, accessToken);
    return retn;
}

void VKContactSyncAdaptor::transformContactAvatars(QList<QContact> &remoteContacts, int accountId, const QString &accessToken)
{
    // The avatar detail from the remote contact will be some remote URL.
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
                if (av.value(QContactAvatar::FieldMetaData).toString() == QStringLiteral("picture")) {
                    avatar = av;
                    break;
                }
            }
            QString remoteImageUrl = avatar.imageUrl().toString();
            if (!remoteImageUrl.isEmpty() && !avatar.imageUrl().isLocalFile()) {
                // transform to a local file name.
                QString localFileName = VKContactImageDownloader::staticOutputFile(
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
                        saveNonexportableDetail(curr, avatar);
                    } else {
                        // not a valid image file.  Could be artifact from an error.
                        QFile::remove(localFileName);
                    }
                }

                if (!QFile::exists(localFileName)) {
                    // temporarily remove the avatar from the contact
                    curr.removeDetail(&avatar);
                    // then trigger the download
                    queueAvatarForDownload(accountId, accessToken, contactGuid, remoteImageUrl);
                }
            }
        }
    }
}

void VKContactSyncAdaptor::imageDownloaded(const QString &url, const QString &path, const QVariantMap &metadata)
{
    Q_UNUSED(url)

    // Load finished, update the avatar, decrement semaphore
    int accountId = metadata.value(IMAGE_DOWNLOADER_ACCOUNT_ID_KEY).toInt();
    QString contactGuid = metadata.value(IMAGE_DOWNLOADER_IDENTIFIER_KEY).toString();

    // Empty path signifies that an error occurred.
    if (!path.isEmpty()) {
        // no longer outstanding.
        m_queuedAvatarsForDownload[accountId].remove(contactGuid);
        m_downloadedContactAvatars[accountId].insert(contactGuid, path);
    }

    decrementSemaphore(accountId);
}

void VKContactSyncAdaptor::deleteDownloadedAvatar(const QContact &contact)
{
    const QString contactGuid = contact.detail<QContactGuid>().guid();
    if (contactGuid.isEmpty()) {
        return;
    }
    const QContactAvatar avatar = contact.detail<QContactAvatar>();
    if (avatar.isEmpty()) {
        return;
    }

    const QString localFileName = VKContactImageDownloader::staticOutputFile(
                contactGuid, avatar.imageUrl().toString());
    if (!localFileName.isEmpty() && QFile::remove(localFileName)) {
        qCDebug(lcSocialPlugin) << "Removed avatar" << localFileName << "of deleted contact" << contact.id();
    }
}

void VKContactSyncAdaptor::finalize(int accountId)
{
    if (syncAborted()) {
        qCDebug(lcSocialPlugin) << "sync aborted, skipping finalize of VK contacts from account:" << accountId;
        m_sqliteSync[accountId]->syncFinishedWithError();
    } else {
        qCDebug(lcSocialPlugin) << "finalizing VK contacts sync with account:" << accountId;
        // first, ensure we update any avatars required.
        if (m_downloadedContactAvatars[accountId].size()) {
            // load all VK contacts from the database.  We need all details, to avoid clobber.
            QContactCollectionFilter collectionFilter;
            collectionFilter.setCollectionId(m_sqliteSync[accountId]->m_collection.id());
            QList<QContact> VKContacts = m_contactManager->contacts(collectionFilter);

            // find the contacts we need to update.
            QMap<QString, QContact> contactsToSave;
            for (auto it = m_downloadedContactAvatars[accountId].constBegin();
                    it != m_downloadedContactAvatars[accountId].constEnd(); ++it) {
                QContact c = findContact(VKContacts, it.key());
                if (c.isEmpty()) {
                    c = findContact(m_remoteContacts[accountId], it.key());
                }
                if (c.isEmpty()) {
                    qCWarning(lcSocialPlugin) << "Not saving avatar, cannot find contact with guid" << it.key();
                } else {
                    // we have downloaded the avatar for this contact, and need to update it.
                    QContactAvatar a;
                    Q_FOREACH (const QContactAvatar &av, c.details<QContactAvatar>()) {
                        if (av.value(QContactAvatar::FieldMetaData).toString() == QStringLiteral("picture")) {
                            a = av;
                            break;
                        }
                    }
                    a.setValue(QContactAvatar::FieldMetaData, QVariant::fromValue<QString>(QStringLiteral("picture")));
                    a.setImageUrl(it.value());
                    saveNonexportableDetail(c, a);
                    contactsToSave[c.detail<QContactGuid>().guid()] = c;
                }
            }

            QList<QContact> saveList = contactsToSave.values();
            if (m_contactManager->saveContacts(&saveList)) {
                qCInfo(lcSocialPlugin) << "finalize: added avatars for" << saveList.size() << "VK contacts from account" << accountId;
            } else {
                qCWarning(lcSocialPlugin) << "finalize: error adding avatars for" << saveList.size() << "VK contacts from account" << accountId;
            }
        }

        m_sqliteSync[accountId]->syncFinishedSuccessfully();
    }
}

void VKContactSyncAdaptor::finalCleanup()
{
    // Synchronously find any contacts which need to be removed,
    // which were somehow "left behind" by the sync process.

    // first, get a list of all existing VK account ids
    QList<int> VKAccountIds;
    QList<int> purgeAccountIds;
    QList<int> currentAccountIds;
    QList<uint> uaids = m_accountManager->accountList();
    foreach (uint uaid, uaids) {
        currentAccountIds.append(static_cast<int>(uaid));
    }
    foreach (int currId, currentAccountIds) {
        Accounts::Account *act = Accounts::Account::fromId(m_accountManager, currId, this);
        if (act) {
            if (act->providerName() == QString(QLatin1String("vk"))) {
                // this account still exists, no need to purge its content
                VKAccountIds.append(currId);
            }
            act->deleteLater();
        }
    }

    // find all account ids from which contacts have been synced
    const QList<QContactCollection> collections = findAllCollections(*m_contactManager, 0);
    for (const QContactCollection &collection : collections) {
        if (collection.metaData(QContactCollection::KeyName).toString() == FriendCollectionName) {
            const int purgeId = collection.extendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_ACCOUNTID).toInt();
            if (purgeId && !VKAccountIds.contains(purgeId)
                    && !purgeAccountIds.contains(purgeId)) {
                // this account no longer exists, and needs to be purged.
                purgeAccountIds.append(purgeId);
            }
        }
    }

    // purge all data for those account ids which no longer exist.
    if (purgeAccountIds.size()) {
        qCInfo(lcSocialPlugin) << "finalCleanup() purging contacts from" << purgeAccountIds.size() << "non-existent VK accounts";
        for (int purgeId : purgeAccountIds) {
            purgeDataForOldAccount(purgeId, SocialNetworkSyncAdaptor::SyncPurge);
        }
    }

    qDeleteAll(m_sqliteSync.values());
    m_sqliteSync.clear();
}
