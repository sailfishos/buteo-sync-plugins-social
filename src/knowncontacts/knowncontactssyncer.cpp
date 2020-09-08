/*
 * Buteo sync plugin that stores locally created contacts
 * Copyright (C) 2020 Open Mobile Platform LLC.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation
 * and appearing in the file LICENSE.LGPL included in the packaging
 * of this file.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301  USA
 */

#include <buteosyncfw5/LogMacros.h>

#include <QContactEmailAddress>
#include <QContactNickname>
#include <QContactOrganization>
#include <QContactCollectionFilter>

#include <QDir>
#include <QLockFile>
#include <QStandardPaths>
#include <QDebug>

#include <qtcontacts-extensions_manager_impl.h>
#include <twowaycontactsyncadaptor_impl.h>
#include <qcontactstatusflags_impl.h>

#include "knowncontactssyncer.h"

/*
  This performs a one-way sync to read contacts from a specified .ini file and write them to the
  contacts database. The .ini files are populated from a GAL address book linked to ActiveSync
  Exchange accounts.
*/

const auto KnownContactsCollectionName = QLatin1String("knowncontacts");
const auto CollectionKeyLastSync = QLatin1String("last-sync-time");

static void setGuid(QContact *contact, const QString &id);
static void setNames(QContact *contact, const QSettings &file);
static void setPhoneNumbers(QContact *contact, const QSettings &file);
static void setEmailAddress(QContact *contact, const QSettings &file);
static void setCompanyInfo(QContact *contact, const QSettings &file);

namespace {

QContactCollection findCollection(const QContactManager &contactManager, const QString &name)
{
    const QList<QContactCollection> collections = contactManager.collections();
    for (const QContactCollection &collection : collections) {
        if (collection.metaData(QContactCollection::KeyName).toString() == name) {
            return collection;
        }
    }
    return QContactCollection();
}

QMap<QString, QString> managerParameters()
{
    QMap<QString, QString> rv;
    // Report presence changes independently from other contact changes
    rv.insert(QString::fromLatin1("mergePresenceChanges"), QString::fromLatin1("false"));
    return rv;
}

}

KnownContactsSyncer::KnownContactsSyncer(QString path, QObject *parent)
    : QObject(parent)
    , QtContactsSqliteExtensions::TwoWayContactSyncAdaptor(0, qAppName(), managerParameters())
    , m_syncFolder(path)
{
    FUNCTION_CALL_TRACE;

    m_collection = findCollection(contactManager(), KnownContactsCollectionName);
    if (m_collection.id().isNull()) {
        LOG_DEBUG("No KnownContacts collection saved yet");

        m_collection.setMetaData(QContactCollection::KeyName, KnownContactsCollectionName);
        m_collection.setMetaData(QContactCollection::KeyDescription, QStringLiteral("EAS GAL contacts"));
        m_collection.setMetaData(QContactCollection::KeyColor, QStringLiteral("yellow"));
        m_collection.setMetaData(QContactCollection::KeySecondaryColor, QStringLiteral("lightyellow"));
        m_collection.setExtendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_APPLICATIONNAME, QCoreApplication::applicationName());
        m_collection.setExtendedMetaData(QStringLiteral("ReadOnly"), true);
    } else {
        LOG_DEBUG("Found KnownContacts collection:" << m_collection.id());
    }
}

KnownContactsSyncer::~KnownContactsSyncer()
{
    FUNCTION_CALL_TRACE;
}

bool KnownContactsSyncer::determineRemoteCollections()
{
    remoteCollectionsDetermined(QList<QContactCollection>() << m_collection);
    return true;
}

bool KnownContactsSyncer::deleteRemoteCollection(const QContactCollection &collection)
{
    Q_UNUSED(collection)

    LOG_WARNING("Collection deletion not supported, ignoring request to delete collection" << collection.id());
    return true;
}

bool KnownContactsSyncer::determineRemoteContacts(const QContactCollection &collection)
{
    FUNCTION_CALL_TRACE;

    const QDateTime remoteSince = collection.extendedMetaData(CollectionKeyLastSync).toDateTime();
    QDir syncDir(m_syncFolder);

    QContactCollectionFilter collectionFilter;
    collectionFilter.setCollectionId(collection.id());
    QContactFetchHint noRelationships;
    noRelationships.setOptimizationHints(QContactFetchHint::NoRelationships);
    const QList<QContact> existingContacts = contactManager().contacts(collectionFilter, QList<QContactSortOrder>(), noRelationships);
    LOG_DEBUG("Found" << existingContacts.size() << "existing contacts");

    QHash<QString, QContact> existingContactsHash;
    for (const QContact &contact : existingContacts) {
        existingContactsHash.insert(contact.detail<QContactGuid>().guid(), contact);
    }

    const QStringList files = syncDir.entryList(QStringList() << "*.ini", QDir::Files);
    for (const auto &file : files) {
        QFileInfo info(syncDir, file);
        if (info.lastModified() >= remoteSince) {
            QSettings settings(info.absoluteFilePath(), QSettings::IniFormat);
            readContacts(&settings, &existingContactsHash);
        }
    }

    for (const auto &file : files) {
        const QString path = syncDir.absoluteFilePath(file);
        if (!QLockFile(path + QStringLiteral(".lock")).tryLock()) {
            LOG_DEBUG("File in use, not removing" << path);
        } else if (!QFile::remove(path)) {
            LOG_WARNING("Could not remove" << path);
        }
    }

    const QList<QContact> updatedContacts = existingContactsHash.values();
    LOG_DEBUG("Reporting" << updatedContacts.size() << "contacts in total");

    remoteContactsDetermined(collection, updatedContacts);
    return true;
}

bool KnownContactsSyncer::storeLocalChangesRemotely(const QContactCollection &collection,
                                                    const QList<QContact> &addedContacts,
                                                    const QList<QContact> &modifiedContacts,
                                                    const QList<QContact> &deletedContacts)
{
    Q_UNUSED(collection)
    Q_UNUSED(addedContacts)
    Q_UNUSED(modifiedContacts)
    Q_UNUSED(deletedContacts)

    LOG_DEBUG("Sync is one-way, ignoring remote changes for" << collection.id());
    return true;
}


void KnownContactsSyncer::storeRemoteChangesLocally(const QContactCollection &collection,
                                                    const QList<QContact> &addedContacts,
                                                    const QList<QContact> &modifiedContacts,
                                                    const QList<QContact> &deletedContacts)
{
    Q_UNUSED(collection)

    m_collection.setExtendedMetaData(CollectionKeyLastSync, QDateTime::currentDateTimeUtc());

    QtContactsSqliteExtensions::TwoWayContactSyncAdaptor::storeRemoteChangesLocally(m_collection, addedContacts, modifiedContacts, deletedContacts);
}

template <typename T>
static inline T findDetail(QContact &contact, int field, const QString &value)
{
    T result;
    QList<T> details = contact.details<T>();
    for (T &detail : details) {
        if (detail.value(field) == value) {
            result = detail;
            break;
        }
    }
    return result;
}

static void setGuid(QContact *contact, const QString &id)
{
    Q_ASSERT(contact);
    auto detail = contact->detail<QContactGuid>();
    detail.setGuid(id);
    contact->saveDetail(&detail);
}

static void setNames(QContact *contact, const QSettings &file)
{
    Q_ASSERT(contact);
    const auto firstName = file.value("FirstName").toString();
    const auto lastName = file.value("LastName").toString();
    if (!firstName.isEmpty() || !lastName.isEmpty()) {
        auto detail = contact->detail<QContactName>();
        if (!firstName.isEmpty())
            detail.setFirstName(firstName);
        if (!lastName.isEmpty())
            detail.setLastName(lastName);
        contact->saveDetail(&detail);
    }
}

// Using QVariant as optional (aka 'maybe') type
static inline void addPhoneNumberDetail(QContact *contact, const QString &value,
                                        const QVariant subType, const QVariant context)
{
    Q_ASSERT(contact);
    if (!value.isEmpty()) {
        auto detail = findDetail<QContactPhoneNumber>(*contact, QContactPhoneNumber::FieldNumber, value);
        detail.setValue(QContactPhoneNumber::FieldNumber, value);
        if (subType.isValid())
            detail.setSubTypes({subType.value<int>()});
        if (context.isValid())
            detail.setContexts({context.value<int>()});
        contact->saveDetail(&detail);
    }
}

static void setPhoneNumbers(QContact *contact, const QSettings &file)
{
    Q_ASSERT(contact);
    addPhoneNumberDetail(contact, file.value("Phone").toString(),
                         QContactPhoneNumber::SubTypeLandline, QVariant());
    addPhoneNumberDetail(contact, file.value("HomePhone").toString(),
                         QContactPhoneNumber::SubTypeLandline, QContactDetail::ContextHome);
    addPhoneNumberDetail(contact, file.value("MobilePhone").toString(),
                         QContactPhoneNumber::SubTypeMobile, QVariant());
}

static void setEmailAddress(QContact *contact, const QSettings &file)
{
    Q_ASSERT(contact);
    const auto emailAddress = file.value("EmailAddress").toString();
    if (!emailAddress.isEmpty()) {
        auto detail = findDetail<QContactEmailAddress>(
                *contact, QContactEmailAddress::FieldEmailAddress, emailAddress);
        detail.setValue(QContactEmailAddress::FieldEmailAddress, emailAddress);
        contact->saveDetail(&detail);
    }
}

static void setCompanyInfo(QContact *contact, const QSettings &file)
{
    Q_ASSERT(contact);
    const auto company = file.value("Company").toString();
    const auto title = file.value("Title").toString();
    const auto office = file.value("Office").toString();
    if (!title.isEmpty() || !office.isEmpty()) {
        auto detail = contact->detail<QContactOrganization>();
        if (!company.isEmpty())
            detail.setName(company);
        if (!title.isEmpty())
            detail.setTitle(title);
        if (!office.isEmpty())
            detail.setLocation(office);
        contact->saveDetail(&detail);
    }
}

void KnownContactsSyncer::readContacts(QSettings *file, QHash<QString, QContact> *contacts)
{
    FUNCTION_CALL_TRACE;

    /*
     * This was implemented to support certain subset of contact fields
     * but can be extended to support more as long as the fields are
     * kept optional.
     */
    for (const auto &id : file->childGroups()) {
        file->beginGroup(id);

        auto it = contacts->find(id);
        if (it == contacts->end()) {
            it = contacts->insert(id, QContact());
            setGuid(&it.value(), id);
        }

        QContact &contact = it.value();
        setNames(&contact, *file);
        setPhoneNumbers(&contact, *file);
        setEmailAddress(&contact, *file);
        setCompanyInfo(&contact, *file);

        file->endGroup();
    }
}

void KnownContactsSyncer::syncFinishedSuccessfully()
{
    LOG_DEBUG("Sync finished OK");
    emit syncSucceeded();
}

void KnownContactsSyncer::syncFinishedWithError()
{
    LOG_WARNING("Sync finished with error");
    emit syncFailed();
}

bool KnownContactsSyncer::purgeData()
{
    const QContactCollectionId collectionId = findCollection(contactManager(), KnownContactsCollectionName).id();
    if (!collectionId.isNull() && !contactManager().removeCollection(collectionId)) {
        LOG_WARNING("Failed to remove contact collection:" << contactManager().error());
        return false;
    }

    LOG_INFO("Successfully removed contact collection" << collectionId);
    return true;
}
