/****************************************************************************
 **
 ** Copyright (c) 2021 Jolla Ltd.
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

#include "googlepeoplejson.h"
#include "googlecontactimagedownloader.h"
#include "trace.h"

#include <qtcontacts-extensions.h>

#include <QContactExtendedDetail>
#include <QContactName>
#include <QContactAddress>
#include <QContactAnniversary>
#include <QContactEmailAddress>
#include <QContactPhoneNumber>
#include <QContactNote>
#include <QContactOrganization>
#include <QContactUrl>
#include <QContactAvatar>
#include <QContactBirthday>
#include <QContactGuid>
#include <QContactNickname>
#include <QContactDisplayLabel>
#include <QContactFavorite>
#include <QFile>

#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>


namespace {

static const QString StarredContactGroupName = QStringLiteral("contactGroups/starred");

QDate jsonObjectToDate(const QJsonObject &object, bool *ok)
{
    const int year = object.value("year").toInt();
    const int month = object.value("month").toInt();
    const int day = object.value("day").toInt();

    QDate date(year, month, day);
    if (!date.isValid()) {
        qCWarning(lcSocialPlugin) << "Cannot read date from JSON:" << object;
    }
    if (ok) {
        *ok = date.isValid();
    }
    return date;
}

QJsonObject jsonObjectFromDate(const QDate &date)
{
    QJsonObject object;
    if (date.isValid()) {
        object.insert("year", date.year());
        object.insert("month", date.month());
        object.insert("day", date.day());
    }
    return object;
}

template <typename T>
QList<T> jsonArrayToList(const QJsonArray &array)
{
    QList<T> values;
    for (auto it = array.constBegin(); it != array.constEnd(); ++it) {
        bool error = false;
        const T &value = T::fromJsonObject(it->toObject(), &error);
        if (!error) {
            values.append(value);
        }
    }
    return values;
}

template <typename T>
void addJsonValuesForContact(const QString &propertyName,
                             const QContact &contact,
                             QJsonObject *object,
                             QStringList *addedFields)
{
    bool hasChanges = false;
    const QJsonArray array = T::jsonValuesForContact(contact, &hasChanges);
    if (!hasChanges) {
        return;
    }

    object->insert(propertyName, array);
    if (addedFields) {
        addedFields->append(propertyName);
    }
}

bool saveContactExtendedDetail(QContact *contact, const QString &detailName, const QVariant &detailData)
{
    QContactExtendedDetail matchedDetail;
    for (const QContactExtendedDetail &detail : contact->details<QContactExtendedDetail>()) {
        if (detail.name() == detailName) {
            matchedDetail = detail;
            break;
        }
    }

    if (matchedDetail.name().isEmpty()) {
        matchedDetail.setName(detailName);
    }
    matchedDetail.setData(detailData);
    return contact->saveDetail(&matchedDetail, QContact::IgnoreAccessConstraints);
}

QVariant contactExtendedDetail(const QContact &contact, const QString &detailName)
{
    for (const QContactExtendedDetail &detail : contact.details<QContactExtendedDetail>()) {
        if (detail.name() == detailName) {
            return detail.data();
        }
    }
    return QVariant();
}

bool saveContactDetail(QContact *contact, QContactDetail *detail)
{
    detail->setValue(QContactDetail__FieldModifiable, true);
    return contact->saveDetail(detail, QContact::IgnoreAccessConstraints);
}

template <typename T>
bool removeDetails(QContact *contact)
{
    QList<T> details = contact->details<T>();
    for (int i = 0; i < details.count(); ++i) {
        T *detail = &details[i];
        if (!contact->removeDetail(detail)) {
            qCWarning(lcSocialPlugin) << "Unable to remove detail:" << detail;
            return false;
        }
    }
    return true;
}

bool shouldAddDetailChanges(const QContactDetail &detail, bool *hasChanges)
{
    const int changeFlags = detail.value(QContactDetail__FieldChangeFlags).toInt();
    if (changeFlags == 0) {
        return false;
    }

    *hasChanges = true;

    if (changeFlags & QContactDetail__ChangeFlag_IsDeleted) {
        return false;
    }

    // Detail was added or modified
    return true;
}

}

GooglePeople::Source GooglePeople::Source::fromJsonObject(const QJsonObject &object, bool *)
{
    Source ret;
    ret.type = object.value("type").toString();
    ret.id = object.value("id").toString();
    ret.etag = object.value("etag").toString();
    return ret;
}

GooglePeople::FieldMetadata GooglePeople::FieldMetadata::fromJsonObject(const QJsonObject &object)
{
    FieldMetadata ret;
    ret.primary = object.value("primary").toBool();
    ret.verified = object.value("verified").toBool();
    ret.source = Source::fromJsonObject(object.value("source").toObject());
    return ret;
}

bool GooglePeople::Address::saveContactDetails(QContact *contact, const QList<Address> &values)
{
    removeDetails<QContactAddress>(contact);

    for (const Address &address : values) {
        QList<int> contexts;
        if (address.type == QStringLiteral("home")) {
            contexts.append(QContactDetail::ContextHome);
        } else if (address.type == QStringLiteral("work")) {
            contexts.append(QContactDetail::ContextWork);
        } else if (address.type == QStringLiteral("other")) {
            contexts.append(QContactDetail::ContextOther);
        } else {
            // address.type is a custom type, so ignore it. If the user does not change it to a
            // known type, the type will not be upsynced and the custom type will be preserved.
        }

        QContactAddress detail;
        if (!contexts.isEmpty()) {
            detail.setContexts(contexts);
        }
        detail.setStreet(address.streetAddress);
        detail.setPostOfficeBox(address.poBox);
        detail.setLocality(address.city);
        detail.setRegion(address.region);
        detail.setPostcode(address.postalCode);
        detail.setCountry(address.country);

        if (!saveContactDetail(contact, &detail)) {
            return false;
        }
    }

    return true;
}

GooglePeople::Address GooglePeople::Address::fromJsonObject(const QJsonObject &obj, bool *)
{
    Address ret;
    ret.metadata = FieldMetadata::fromJsonObject(obj.value("metadata").toObject());
    ret.formattedValue = obj.value("formattedValue").toString();
    ret.type = obj.value("type").toString();
    ret.formattedType = obj.value("formattedType").toString();
    ret.poBox = obj.value("poBox").toString();
    ret.streetAddress = obj.value("streetAddress").toString();
    ret.extendedAddress = obj.value("extendedAddress").toString();
    ret.city = obj.value("city").toString();
    ret.region = obj.value("region").toString();
    ret.postalCode = obj.value("postalCode").toString();
    ret.country = obj.value("country").toString();
    ret.countryCode = obj.value("countryCode").toString();
    return ret;
}

QJsonArray GooglePeople::Address::jsonValuesForContact(const QContact &contact, bool *hasChanges)
{
    QJsonArray array;
    const QList<QContactAddress> details = contact.details<QContactAddress>();

    for (int i = 0; i < details.count(); ++i) {
        const QContactAddress &detail = details.at(i);
        if (!shouldAddDetailChanges(detail, hasChanges)) {
            continue;
        }

        const int context = detail.contexts().value(0, -1);
        QString type;
        switch (context) {
        case QContactDetail::ContextHome:
            type = QStringLiteral("home");
            break;
        case QContactDetail::ContextWork:
            type = QStringLiteral("work");
            break;
        case QContactDetail::ContextOther:
            type = QStringLiteral("other");
            break;
        }

        QJsonObject address;
        if (type.isEmpty()) {
            // No type set, or the Google field had a custom type set, so don't overwrite it.
        } else {
            address.insert("type", type);
        }
        address.insert("poBox", detail.postOfficeBox());
        address.insert("streetAddress", detail.street());
        address.insert("city", detail.locality());
        address.insert("region", detail.region());
        address.insert("postalCode", detail.postcode());
        address.insert("country", detail.country());
        array.append(address);
    }

    return array;
}

bool GooglePeople::Biography::saveContactDetails(QContact *contact, const QList<Biography> &values)
{
    // Only one biography allowed in a Google contact.
    if (values.isEmpty()) {
        return true;
    }

    QContactNote detail = contact->detail<QContactNote>();
    detail.setNote(values.at(0).value);
    return saveContactDetail(contact, &detail);
}

GooglePeople::Biography GooglePeople::Biography::fromJsonObject(const QJsonObject &object, bool *)
{
    Biography ret;
    ret.metadata = FieldMetadata::fromJsonObject(object.value("metadata").toObject());
    ret.value = object.value("value").toString();
    return ret;
}

QJsonArray GooglePeople::Biography::jsonValuesForContact(const QContact &contact, bool *hasChanges)
{
    // Only one biography allowed in a Google contact.
    QJsonArray array;
    const QContactNote &detail = contact.detail<QContactNote>();
    if (!shouldAddDetailChanges(detail, hasChanges)) {
        return array;
    }

    QJsonObject note;
    note.insert("value", detail.note());
    array.append(note);

    return array;
}

bool GooglePeople::Birthday::saveContactDetails(QContact *contact, const QList<Birthday> &values)
{
    // Only one birthday allowed in a Google contact.
    if (values.isEmpty()) {
        return true;
    }

    QContactBirthday detail = contact->detail<QContactBirthday>();
    detail.setDate(values.at(0).date);
    return saveContactDetail(contact, &detail);
}

GooglePeople::Birthday GooglePeople::Birthday::fromJsonObject(const QJsonObject &object, bool *error)
{
    bool dateOk = false;
    const QDate date = jsonObjectToDate(object.value("date").toObject(), &dateOk);
    if (error) {
        *error = !dateOk;
    }
    if (!dateOk) {
        return Birthday();
    }

    Birthday ret;
    ret.metadata = FieldMetadata::fromJsonObject(object.value("metadata").toObject());
    ret.date = date;
    return ret;
}

QJsonArray GooglePeople::Birthday::jsonValuesForContact(const QContact &contact, bool *hasChanges)
{
    // Only one birthday allowed in a Google contact.
    QJsonArray array;
    const QContactBirthday &detail = contact.detail<QContactBirthday>();
    if (!shouldAddDetailChanges(detail, hasChanges)) {
        return array;
    }

    QJsonObject birthday;
    birthday.insert("date", jsonObjectFromDate(detail.date()));
    array.append(birthday);
    return array;
}

bool GooglePeople::EmailAddress::saveContactDetails(QContact *contact, const QList<EmailAddress> &values)
{
    removeDetails<QContactEmailAddress>(contact);

    QStringList types;
    for (const EmailAddress &emailAddress : values) {
        QList<int> contexts;
        if (emailAddress.type == QStringLiteral("home")) {
            contexts.append(QContactDetail::ContextHome);
        } else if (emailAddress.type == QStringLiteral("work")) {
            contexts.append(QContactDetail::ContextWork);
        } else if (emailAddress.type == QStringLiteral("other")) {
            contexts.append(QContactDetail::ContextOther);
        } else {
            // emailAddress.type is a custom type, so ignore it. If the user does not change it to a
            // known type, the type will not be upsynced and the custom type will be preserved.
        }

        QContactEmailAddress detail;
        if (!contexts.isEmpty()) {
            detail.setContexts(contexts);
        }
        detail.setEmailAddress(emailAddress.value);
        if (!saveContactDetail(contact, &detail)) {
            return false;
        }

        types.append(emailAddress.type);
    }

    return true;
}

GooglePeople::EmailAddress GooglePeople::EmailAddress::fromJsonObject(const QJsonObject &object, bool *)
{
    EmailAddress ret;
    ret.metadata = FieldMetadata::fromJsonObject(object.value("metadata").toObject());
    ret.value = object.value("value").toString();
    ret.type = object.value("type").toString();
    ret.formattedType = object.value("formattedType").toString();
    ret.displayName = object.value("displayName").toString();
    return ret;
}

QJsonArray GooglePeople::EmailAddress::jsonValuesForContact(const QContact &contact, bool *hasChanges)
{
    QJsonArray array;
    const QList<QContactEmailAddress> details = contact.details<QContactEmailAddress>();

    for (int i = 0; i < details.count(); ++i) {
        const QContactEmailAddress &detail = details.at(i);
        if (!shouldAddDetailChanges(detail, hasChanges)) {
            continue;
        }

        const int context = detail.contexts().value(0, -1);
        QString type;
        switch (context) {
        case QContactDetail::ContextHome:
            type = QStringLiteral("home");
            break;
        case QContactDetail::ContextWork:
            type = QStringLiteral("work");
            break;
        case QContactDetail::ContextOther:
            type = QStringLiteral("other");
            break;
        }

        QJsonObject email;
        if (type.isEmpty()) {
            // No type set, or the Google field had a custom type set, so don't overwrite it.
        } else {
            email.insert("type", type);
        }
        email.insert("value", detail.emailAddress());
        array.append(email);
    }
    return array;
}


bool GooglePeople::Event::saveContactDetails(QContact *contact, const QList<Event> &values)
{
    removeDetails<QContactAnniversary>(contact);

    for (const Event &event : values) {
        QContactAnniversary detail;
        detail.setOriginalDateTime(QDateTime(event.date));
        if (event.type == QStringLiteral("Wedding")) {
            detail.setSubType(QContactAnniversary::SubTypeWedding);
        } else if (event.type == QStringLiteral("Engagement")) {
            detail.setSubType(QContactAnniversary::SubTypeEngagement);
        } else if (event.type == QStringLiteral("House")) {
            detail.setSubType(QContactAnniversary::SubTypeHouse);
        } else if (event.type == QStringLiteral("Employment")) {
            detail.setSubType(QContactAnniversary::SubTypeEmployment);
        } else if (event.type == QStringLiteral("Memorial")) {
            detail.setSubType(QContactAnniversary::SubTypeMemorial);
        } else {
            // event.type is a custom type, so ignore it. If the user does not change it to a
            // known type, the type will not be upsynced and the custom type will be preserved.
        }

        if (!saveContactDetail(contact, &detail)) {
            return false;
        }
    }

    return true;
}

GooglePeople::Event GooglePeople::Event::fromJsonObject(const QJsonObject &object, bool *error)
{
    bool dateOk = false;
    const QDate date = jsonObjectToDate(object.value("date").toObject(), &dateOk);
    if (error) {
        *error = !dateOk;
    }
    if (!dateOk) {
        return Event();
    }

    Event ret;
    ret.metadata = FieldMetadata::fromJsonObject(object.value("metadata").toObject());
    ret.date = date;
    ret.type = object.value("type").toString();
    return ret;
}

QJsonArray GooglePeople::Event::jsonValuesForContact(const QContact &contact, bool *hasChanges)
{
    QJsonArray array;
    const QList<QContactAnniversary> details = contact.details<QContactAnniversary>();

    for (int i = 0; i < details.count(); ++i) {
        const QContactAnniversary &detail = details.at(i);
        if (!shouldAddDetailChanges(detail, hasChanges)) {
            continue;
        }

        QString type;
        switch (detail.subType()) {
        case QContactAnniversary::SubTypeWedding:
            type = QStringLiteral("Wedding");
            break;
        case QContactAnniversary::SubTypeEngagement:
            type = QStringLiteral("Engagement");
            break;
        case QContactAnniversary::SubTypeHouse:
            type = QStringLiteral("House");
            break;
        case QContactAnniversary::SubTypeEmployment:
            type = QStringLiteral("Employment");
            break;
        case QContactAnniversary::SubTypeMemorial:
            type = QStringLiteral("Memorial");
            break;
        default:
            break;
        }

        QJsonObject event;
        if (type.isEmpty()) {
            // No type set, or the Google field had a custom type set, so don't overwrite it.
        } else {
            event.insert("type", type);
        }
        event.insert("date", jsonObjectFromDate(detail.originalDateTime().date()));
        array.append(event);
    }

    return array;
}

bool GooglePeople::Membership::matchesCollection(const QContactCollection &collection, int accountId) const
{
    return collection.extendedMetaData(QStringLiteral("resourceName")).toString() == contactGroupResourceName
         && collection.extendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_ACCOUNTID).toInt() == accountId;
}

bool GooglePeople::Membership::saveContactDetails(
        QContact *contact,
        const QList<Membership> &values,
        int accountId,
        const QList<QContactCollection> &candidateCollections)
{
    contact->setCollectionId(QContactCollectionId());

    QStringList contactGroupResourceNames;
    bool isFavorite = false;
    for (const Membership &membership : values) {
        if (contact->collectionId().isNull()) {
            for (const QContactCollection &collection : candidateCollections) {
                if (membership.matchesCollection(collection, accountId)) {
                    contact->setCollectionId(collection.id());
                    break;
                }
            }
        }
        if (membership.contactGroupResourceName == StarredContactGroupName) {
            isFavorite = true;
        }

        contactGroupResourceNames.append(membership.contactGroupResourceName);
    }

    QContactFavorite favoriteDetail = contact->detail<QContactFavorite>();
    favoriteDetail.setFavorite(isFavorite);
    if (!saveContactDetail(contact, &favoriteDetail)) {
        return false;
    }

    // Preserve contactGroupResourceName values since a Person can belong to multiple contact
    // groups but a QContact can only belong to one collection.
    if (!saveContactExtendedDetail(contact, QStringLiteral("contactGroupResourceNames"), contactGroupResourceNames)) {
        return false;
    }

    return true;
}

GooglePeople::Membership GooglePeople::Membership::fromJsonObject(const QJsonObject &object, bool *)
{
    Membership ret;
    ret.metadata = FieldMetadata::fromJsonObject(object.value("metadata").toObject());

    const QJsonObject contactGroupMembership = object.value("contactGroupMembership").toObject();
    ret.contactGroupResourceName = contactGroupMembership.value("contactGroupResourceName").toString();

    return ret;
}

QJsonArray GooglePeople::Membership::jsonValuesForContact(const QContact &contact, bool *hasChanges)
{
    QJsonArray array;
    QStringList contactGroupResourceNames = contactExtendedDetail(
                contact, QStringLiteral("contactGroupResourceNames")).toStringList();

    const QContactFavorite favoriteDetail = contact.detail<QContactFavorite>();
    if (shouldAddDetailChanges(favoriteDetail, hasChanges)) {
        const bool isFavorite = favoriteDetail.isFavorite();
        if (isFavorite && contactGroupResourceNames.indexOf(StarredContactGroupName) < 0) {
            contactGroupResourceNames.append(StarredContactGroupName);
        } else if (!isFavorite) {
            contactGroupResourceNames.removeOne(StarredContactGroupName);
        }
    }

    if (contact.id().isNull()) {
        // This is a new contact, so add its collection into the list of memberships.
        *hasChanges = true;
    }

    if (*hasChanges) {
        // Add the list of all known memberships of this contact.
        for (const QString &contactGroupResourceName : contactGroupResourceNames) {
            QJsonObject membership;
            // Add the nested contactGroupMembership object. Don't need to add "contactGroupId"
            // property as that is deprecated.
            QJsonObject contactGroupMembershipObject;
            contactGroupMembershipObject.insert("contactGroupResourceName", contactGroupResourceName);
            membership.insert("contactGroupMembership", contactGroupMembershipObject);
            array.append(membership);
        }
    }

    return array;
}

bool GooglePeople::Name::saveContactDetails(QContact *contact, const QList<Name> &values)
{
    // Only one name allowed in a Google contact.
    if (values.isEmpty()) {
        return true;
    }

    const Name &name = values.at(0);
    QContactName detail = contact->detail<QContactName>();
    detail.setFirstName(name.givenName);
    detail.setMiddleName(name.middleName);
    detail.setLastName(name.familyName);
    return saveContactDetail(contact, &detail);
}

GooglePeople::Name GooglePeople::Name::fromJsonObject(const QJsonObject &object, bool *)
{
    Name ret;
    ret.metadata = FieldMetadata::fromJsonObject(object.value("metadata").toObject());
    ret.familyName = object.value("familyName").toString();
    ret.givenName = object.value("givenName").toString();
    ret.middleName = object.value("middleName").toString();
    return ret;
}

QJsonArray GooglePeople::Name::jsonValuesForContact(const QContact &contact, bool *hasChanges)
{
    // Only one name allowed in a Google contact.
    QJsonArray array;
    const QContactName &detail = contact.detail<QContactName>();
    if (!shouldAddDetailChanges(detail, hasChanges)) {
        return array;
    }

    QJsonObject name;
    name.insert("familyName", detail.lastName());
    name.insert("givenName", detail.firstName());
    name.insert("middleName", detail.middleName());
    name.insert("honorificPrefix", detail.prefix());
    name.insert("honorificSuffix", detail.suffix());
    array.append(name);

    return array;
}

bool GooglePeople::Nickname::saveContactDetails(QContact *contact, const QList<Nickname> &values)
{
    removeDetails<QContactNickname>(contact);

    for (const Nickname &nickName : values) {
        QContactNickname detail;
        detail.setNickname(nickName.value);
        if (!saveContactDetail(contact, &detail)) {
            return false;
        }
    }

    return true;
}

GooglePeople::Nickname GooglePeople::Nickname::fromJsonObject(const QJsonObject &object, bool *)
{
    Nickname ret;
    ret.metadata = FieldMetadata::fromJsonObject(object.value("metadata").toObject());
    ret.value = object.value("value").toString();
    return ret;
}

QJsonArray GooglePeople::Nickname::jsonValuesForContact(const QContact &contact, bool *hasChanges)
{
    QJsonArray array;
    const QList<QContactNickname> details = contact.details<QContactNickname>();

    for (const QContactNickname &detail : details) {
        if (!shouldAddDetailChanges(detail, hasChanges)) {
            continue;
        }
        QJsonObject nickName;
        nickName.insert("value", detail.nickname());
        array.append(nickName);
    }
    return array;
}

bool GooglePeople::Organization::saveContactDetails(QContact *contact, const QList<Organization> &values)
{
    removeDetails<QContactOrganization>(contact);

    for (const Organization &organization : values) {
        QContactOrganization detail;
        detail.setName(organization.name);
        detail.setTitle(organization.title);
        detail.setRole(organization.jobDescription);
        detail.setDepartment(QStringList(organization.department));
        if (!saveContactDetail(contact, &detail)) {
            return false;
        }
    }

    return true;
}

GooglePeople::Organization GooglePeople::Organization::fromJsonObject(const QJsonObject &object, bool *)
{
    Organization ret;
    ret.metadata = FieldMetadata::fromJsonObject(object.value("metadata").toObject());
    ret.name = object.value("name").toString();
    ret.title = object.value("title").toString();
    ret.jobDescription = object.value("jobDescription").toString();
    ret.department = object.value("department").toString();
    return ret;
}

QJsonArray GooglePeople::Organization::jsonValuesForContact(const QContact &contact, bool *hasChanges)
{
    QJsonArray array;
    const QList<QContactOrganization> details = contact.details<QContactOrganization>();

    for (const QContactOrganization &detail : details) {
        if (!shouldAddDetailChanges(detail, hasChanges)) {
            continue;
        }
        QJsonObject org;
        org.insert("name", detail.name());
        org.insert("title", detail.title());
        org.insert("jobDescription", detail.role());
        org.insert("department", detail.department().value(0));
        array.append(org);
    }

    return array;
}

bool GooglePeople::PhoneNumber::saveContactDetails(QContact *contact, const QList<PhoneNumber> &values)
{
    removeDetails<QContactPhoneNumber>(contact);

    for (const PhoneNumber &phoneNumber : values) {
        QContactPhoneNumber detail;
        detail.setNumber(phoneNumber.value);

        if (phoneNumber.type == QStringLiteral("home")) {
            detail.setContexts(QContactDetail::ContextHome);
        } else if (phoneNumber.type == QStringLiteral("work")) {
            detail.setContexts(QContactDetail::ContextWork);
        } else if (phoneNumber.type == QStringLiteral("mobile")) {
            detail.setSubTypes(QList<int>() << QContactPhoneNumber::SubTypeMobile);
        } else if (phoneNumber.type == QStringLiteral("workMobile")) {
            detail.setContexts(QContactDetail::ContextWork);
            detail.setSubTypes(QList<int>() << QContactPhoneNumber::SubTypeMobile);
        } else if (phoneNumber.type == QStringLiteral("homeFax")) {
            detail.setContexts(QContactDetail::ContextHome);
            detail.setSubTypes(QList<int>() << QContactPhoneNumber::SubTypeFax);
        } else if (phoneNumber.type == QStringLiteral("workFax")) {
            detail.setContexts(QContactDetail::ContextWork);
            detail.setSubTypes(QList<int>() << QContactPhoneNumber::SubTypeFax);
        } else if (phoneNumber.type == QStringLiteral("otherFax")) {
            detail.setContexts(QContactDetail::ContextOther);
            detail.setSubTypes(QList<int>() << QContactPhoneNumber::SubTypeFax);
        } else if (phoneNumber.type == QStringLiteral("pager")) {
            detail.setSubTypes(QList<int>() << QContactPhoneNumber::SubTypePager);
        } else if (phoneNumber.type == QStringLiteral("workPager")) {
            detail.setContexts(QContactDetail::ContextWork);
            detail.setSubTypes(QList<int>() << QContactPhoneNumber::SubTypePager);
        } else if (phoneNumber.type == QStringLiteral("other")) {
            detail.setContexts(QContactDetail::ContextOther);
        } else {
            // phoneNumber.type is a custom type, so ignore it. If the user does not change it to a
            // known type, the type will not be upsynced and the custom type will be preserved.
        }

        if (!saveContactDetail(contact, &detail)) {
            return false;
        }
    }

    return true;
}

GooglePeople::PhoneNumber GooglePeople::PhoneNumber::fromJsonObject(const QJsonObject &object, bool *)
{
    PhoneNumber ret;
    ret.metadata = FieldMetadata::fromJsonObject(object.value("metadata").toObject());
    ret.value = object.value("value").toString();
    ret.type = object.value("type").toString();
    return ret;
}

QJsonArray GooglePeople::PhoneNumber::jsonValuesForContact(const QContact &contact, bool *hasChanges)
{
    QJsonArray array;
    const QList<QContactPhoneNumber> details = contact.details<QContactPhoneNumber>();

    for (int i = 0; i < details.count(); ++i) {
        const QContactPhoneNumber &detail = details.at(i);
        if (!shouldAddDetailChanges(detail, hasChanges)) {
            continue;
        }

        QString type;
        const int context = detail.contexts().value(0, -1);
        if (detail.subTypes().isEmpty()) {
            if (context == QContactDetail::ContextHome) {
                type = QStringLiteral("home");
            } else if (context == QContactDetail::ContextWork) {
                type = QStringLiteral("work");
            }
        } else {
            const int subType = detail.subTypes().at(0);
            switch (subType) {
            case QContactPhoneNumber::SubTypeMobile:
                type = QStringLiteral("mobile");
                break;
            case QContactPhoneNumber::SubTypeFax:
                if (context ==  QContactDetail::ContextHome) {
                    type = QStringLiteral("homeFax");
                } else if (context ==  QContactDetail::ContextWork) {
                    type = QStringLiteral("workFax");
                } else if (context ==  QContactDetail::ContextOther) {
                    type = QStringLiteral("otherFax");
                }
                break;
            case QContactPhoneNumber::SubTypePager:
                if (context ==  QContactDetail::ContextWork) {
                    type = QStringLiteral("workPager");
                } else {
                    type = QStringLiteral("pager");
                }
                break;
            default:
                break;
            }
        }

        QJsonObject phone;
        if (type.isEmpty()) {
            // No type set, or the Google field had a custom type set, so don't overwrite it.
        } else {
            phone.insert("type", type);
        }
        phone.insert("value", detail.number());
        array.append(phone);
    }
    return array;
}

bool GooglePeople::PersonMetadata::saveContactDetails(QContact *contact, const PersonMetadata &metadata)
{
    for (const Source &source : metadata.sources) {
        QVariantMap sourceInfo;
        sourceInfo.insert("type", source.type);
        sourceInfo.insert("id", source.id);
        sourceInfo.insert("etag", source.etag);
        if (!saveContactExtendedDetail(contact, QStringLiteral("source_%1").arg(source.type), sourceInfo)) {
            return false;
        }
    }
    return true;
}

QString GooglePeople::PersonMetadata::etag(const QContact &contact)
{
    const QVariantMap sourceInfo = contactExtendedDetail(contact, QStringLiteral("source_CONTACT")).toMap();
    return sourceInfo.value("etag").toString();
}

GooglePeople::PersonMetadata GooglePeople::PersonMetadata::fromJsonObject(const QJsonObject &object, bool *)
{
    PersonMetadata ret;
    ret.sources = jsonArrayToList<Source>(object.value("sources").toArray());
    ret.previousResourceNames = object.value("previousResourceNames").toVariant().toStringList();
    ret.linkedPeopleResourceNames = object.value("linkedPeopleResourceNames").toVariant().toStringList();
    ret.deleted = object.value("deleted").toBool();
    return ret;
}

QJsonObject GooglePeople::PersonMetadata::toJsonObject(const QContact &contact)
{
    // Only need to add the details for the "CONTACT" source.
    QJsonObject metadataObject;
    const QVariantMap sourceInfo = contactExtendedDetail(
                contact, QStringLiteral("source_CONTACT")).toMap();

    if (!sourceInfo.isEmpty()) {
        QJsonObject sourceObject;
        sourceObject.insert("type", sourceInfo.value("type").toString());
        sourceObject.insert("id", sourceInfo.value("id").toString());
        sourceObject.insert("etag", sourceInfo.value("etag").toString());

        QJsonArray sourcesArray;
        sourcesArray.append(QJsonValue(sourceObject));

        metadataObject.insert("sources", sourcesArray);
    }

    return metadataObject;
}

QContactAvatar GooglePeople::Photo::getPrimaryPhoto(const QContact &contact,
                                                    QString *remoteAvatarUrl,
                                                    QString *localAvatarFile)
{
    // Use the first avatar as the the primary photo for the contact.
    const QContactAvatar avatar = contact.detail<QContactAvatar>();
    if (localAvatarFile) {
        *localAvatarFile = avatar.imageUrl().toString();
    }
    if (remoteAvatarUrl) {
        *remoteAvatarUrl = avatar.videoUrl().toString();
    }

    return avatar;
}

bool GooglePeople::Photo::saveContactDetails(QContact *contact, const QList<Photo> &values)
{
    removeDetails<QContactAvatar>(contact);

    const QString guid = contact->detail<QContactGuid>().guid();

    for (const Photo &photo : values) {
        if (photo.default_) {
            // Ignore the Google-generated avatar that simply shows the contact's initials.
            continue;
        }

        QContactAvatar avatar;
        const QString localFilePath = GoogleContactImageDownloader::staticOutputFile(guid, photo.url);
        if (localFilePath.isEmpty()) {
            qCWarning(lcSocialPlugin) << "Cannot generate local file name for avatar url:" << photo.url
                              << "for contact:" << guid;
            continue;
        }

        avatar.setImageUrl(QUrl(localFilePath));
        avatar.setVideoUrl(QUrl(photo.url));    // ugly hack to store the remote url separately to the local path

        if (!saveContactDetail(contact, &avatar)) {
            return false;
        }
    }

    return true;
}

GooglePeople::Photo GooglePeople::Photo::fromJsonObject(const QJsonObject &object, bool *)
{
    Photo ret;
    ret.metadata = FieldMetadata::fromJsonObject(object.value("metadata").toObject());
    ret.url = object.value("url").toString();
    ret.default_ = object.value("default").toBool();
    return ret;
}

QJsonArray GooglePeople::Photo::jsonValuesForContact(const QContact &contact, bool *hasChanges)
{
    QJsonArray array;
    const QList<QContactAvatar> details = contact.details<QContactAvatar>();

    for (const QContactAvatar &detail : details) {
        if (!shouldAddDetailChanges(detail, hasChanges)) {
            continue;
        }
        QJsonObject photo;
        photo.insert("url", detail.imageUrl().toString());
        array.append(photo);
    }
    return array;
}

bool GooglePeople::Url::saveContactDetails(QContact *contact, const QList<Url> &values)
{
    removeDetails<QContactUrl>(contact);

    for (const Url &url : values) {
        QContactUrl detail;
        detail.setUrl(url.value);

        if (url.type == QStringLiteral("homePage")) {
            detail.setSubType(QContactUrl::SubTypeHomePage);
        } else if (url.type == QStringLiteral("blog")) {
            detail.setSubType(QContactUrl::SubTypeBlog);
        }

        if (!saveContactDetail(contact, &detail)) {
            return false;
        }
    }

    return true;
}

GooglePeople::Url GooglePeople::Url::fromJsonObject(const QJsonObject &object, bool *)
{
    Url ret;
    ret.metadata = FieldMetadata::fromJsonObject(object.value("metadata").toObject());
    ret.value = object.value("value").toString();
    ret.type = object.value("type").toString();
    ret.formattedType = object.value("formattedType").toString();
    return ret;
}

QJsonArray GooglePeople::Url::jsonValuesForContact(const QContact &contact, bool *hasChanges)
{
    QJsonArray array;
    const QList<QContactUrl> details = contact.details<QContactUrl>();

    for (const QContactUrl &detail : details) {
        if (!shouldAddDetailChanges(detail, hasChanges)) {
            continue;
        }
        QJsonObject url;
        switch (detail.subType()) {
        case QContactUrl::SubTypeHomePage:
            url.insert("type", QStringLiteral("homePage"));
            break;
        case QContactUrl::SubTypeBlog:
            url.insert("type", QStringLiteral("blog"));
            break;
        default:
            // No type set, or the Google field had a custom type set, so don't overwrite it.
            break;
        }
        url.insert("value", detail.url());
        array.append(url);
    }
    return array;
}

QContact GooglePeople::Person::toContact(int accountId,
                                         const QList<QContactCollection> &candidateCollections) const
{
    QContact contact;
    saveToContact(&contact, accountId, candidateCollections);
    return contact;
}

bool GooglePeople::Person::saveToContact(QContact *contact,
                                         int accountId,
                                         const QList<QContactCollection> &candidateCollections) const
{
    if (!contact) {
        qCWarning(lcSocialPlugin) << "saveToContact() failed: invalid contact!";
        return false;
    }

    QContactGuid guid = contact->detail<QContactGuid>();
    if (guid.guid().isEmpty()) {
        guid.setGuid(guidForPerson(accountId, resourceName));
        if (!contact->saveDetail(&guid, QContact::IgnoreAccessConstraints)) {
            return false;
        }
    }

    PersonMetadata::saveContactDetails(contact, metadata);
    Address::saveContactDetails(contact, addresses);
    Biography::saveContactDetails(contact, biographies);
    Birthday::saveContactDetails(contact, birthdays);
    EmailAddress::saveContactDetails(contact, emailAddresses);
    Event::saveContactDetails(contact, events);
    Membership::saveContactDetails(contact, memberships, accountId, candidateCollections);
    Name::saveContactDetails(contact, names);
    Nickname::saveContactDetails(contact, nicknames);
    Organization::saveContactDetails(contact, organizations);
    PhoneNumber::saveContactDetails(contact, phoneNumbers);
    Photo::saveContactDetails(contact, photos);
    Url::saveContactDetails(contact, urls);

    return true;
}

GooglePeople::Person GooglePeople::Person::fromJsonObject(const QJsonObject &object)
{
    Person ret;
    ret.resourceName = object.value("resourceName").toString();
    ret.metadata = PersonMetadata::fromJsonObject(object.value("metadata").toObject(), nullptr);
    ret.addresses = jsonArrayToList<Address>(object.value("addresses").toArray());
    ret.biographies = jsonArrayToList<Biography>(object.value("biographies").toArray());
    ret.birthdays = jsonArrayToList<Birthday>(object.value("birthdays").toArray());
    ret.emailAddresses = jsonArrayToList<EmailAddress>(object.value("emailAddresses").toArray());
    ret.events = jsonArrayToList<Event>(object.value("events").toArray());
    ret.memberships = jsonArrayToList<Membership>(object.value("memberships").toArray());
    ret.names = jsonArrayToList<Name>(object.value("names").toArray());
    ret.nicknames = jsonArrayToList<Nickname>(object.value("nicknames").toArray());
    ret.organizations = jsonArrayToList<Organization>(object.value("organizations").toArray());
    ret.phoneNumbers = jsonArrayToList<PhoneNumber>(object.value("phoneNumbers").toArray());
    ret.photos = jsonArrayToList<Photo>(object.value("photos").toArray());
    ret.urls = jsonArrayToList<Url>(object.value("urls").toArray());
    return ret;
}

GooglePeople::ContactGroupMetadata GooglePeople::ContactGroupMetadata::fromJsonObject(const QJsonObject &obj)
{
    ContactGroupMetadata ret;
    const QString updateTime = obj.value("updateTime").toString();
    if (!updateTime.isEmpty()) {
        ret.updateTime = QDateTime::fromString(updateTime, Qt::ISODate);
    }
    ret.deleted = obj.value("deleted").toBool();
    return ret;
}

QJsonObject GooglePeople::Person::contactToJsonObject(const QContact &contact,
                                                      QStringList *addedFields)
{
    QJsonObject person;

    // Add resourceName
    QString resourceName = personResourceName(contact);
    if (!resourceName.isEmpty()) {
        person.insert("resourceName", resourceName);
    }

    // Add metadata including etag
    QJsonObject metadataObject = PersonMetadata::toJsonObject(contact);
    if (!metadataObject.isEmpty()) {
        person.insert("metadata", metadataObject);
    }

    // Add other fields.
    // photos are not added here, as they can only modified in the Google People API via
    // updateContactPhoto(), and cannot be passed in createContact() and updateContact().
    addJsonValuesForContact<Address>(QStringLiteral("addresses"),
                            contact, &person, addedFields);
    addJsonValuesForContact<Biography>(QStringLiteral("biographies"),
                            contact, &person, addedFields);
    addJsonValuesForContact<Birthday>(QStringLiteral("birthdays"),
                            contact, &person, addedFields);
    addJsonValuesForContact<EmailAddress>(QStringLiteral("emailAddresses"),
                            contact, &person, addedFields);
    addJsonValuesForContact<Event>(QStringLiteral("events"),
                            contact, &person, addedFields);
    addJsonValuesForContact<Membership>(QStringLiteral("memberships"),
                            contact, &person, addedFields);
    addJsonValuesForContact<Name>(QStringLiteral("names"),
                            contact, &person, addedFields);
    addJsonValuesForContact<Nickname>(QStringLiteral("nicknames"),
                            contact, &person, addedFields);
    addJsonValuesForContact<Organization>(QStringLiteral("organizations"),
                            contact, &person, addedFields);
    addJsonValuesForContact<PhoneNumber>(QStringLiteral("phoneNumbers"),
                            contact, &person, addedFields);
    addJsonValuesForContact<Url>(QStringLiteral("urls"),
                            contact, &person, addedFields);

    return person;
}

QString GooglePeople::Person::personResourceName(const QContact &contact)
{
    const QString guid = contact.detail<QContactGuid>().guid();
    if (!guid.isEmpty()) {
        const int index = guid.indexOf(':');
        if (index >= 0) {
            return guid.mid(index + 1);
        }
    }
    return QString();
}

QStringList GooglePeople::Person::supportedPersonFields()
{
    static QStringList fields;
    if (fields.isEmpty()) {
        fields << QStringLiteral("metadata");
        fields << QStringLiteral("addresses");
        fields << QStringLiteral("biographies");
        fields << QStringLiteral("birthdays");
        fields << QStringLiteral("emailAddresses");
        fields << QStringLiteral("events");
        fields << QStringLiteral("memberships");
        fields << QStringLiteral("names");
        fields << QStringLiteral("nicknames");
        fields << QStringLiteral("organizations");
        fields << QStringLiteral("phoneNumbers");
        fields << QStringLiteral("photos");
        fields << QStringLiteral("urls");
    }
    return fields;
}

QString GooglePeople::Person::guidForPerson(int accountId, const QString &resourceName)
{
    return QStringLiteral("%1:%2").arg(accountId).arg(resourceName);
}

bool GooglePeople::ContactGroup::isMyContactsGroup() const
{
    return resourceName == QStringLiteral("contactGroups/myContacts");
}

QContactCollection GooglePeople::ContactGroup::toCollection(int accountId) const
{
    QContactCollection collection;
    collection.setMetaData(QContactCollection::KeyName, formattedName);
    collection.setExtendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_APPLICATIONNAME, QCoreApplication::applicationName());
    collection.setExtendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_ACCOUNTID, accountId);
    collection.setExtendedMetaData(QStringLiteral("resourceName"), resourceName);
    collection.setExtendedMetaData(QStringLiteral("groupType"), groupType);

    return collection;
}

bool GooglePeople::ContactGroup::isMyContactsCollection(const QContactCollection &collection, int accountId)
{
    return collection.extendedMetaData("resourceName").toString() == QStringLiteral("contactGroups/myContacts")
        && (accountId == 0
            || collection.extendedMetaData(COLLECTION_EXTENDEDMETADATA_KEY_ACCOUNTID).toInt() == accountId);
}

GooglePeople::ContactGroup GooglePeople::ContactGroup::fromJsonObject(const QJsonObject &obj)
{
    ContactGroup ret;
    ret.resourceName = obj.value("resourceName").toString();
    ret.etag = obj.value("etag").toString();
    ret.contactGroupMetadata = ContactGroupMetadata::fromJsonObject(obj.value("contactGroupMetadata").toObject());
    ret.groupType = obj.value("groupType").toString();
    ret.name = obj.value("name").toString();
    ret.formattedName = obj.value("formattedName").toString();
    ret.memberResourceNames = obj.value("memberResourceNames").toVariant().toStringList();
    ret.memberCount = obj.value("memberCount").toInt();
    return ret;
}

#define DEBUG_VALUE_ONLY(propertyName) \
    debug.nospace() << #propertyName << "=" << value.propertyName

#define DEBUG_VALUE(propertyName) \
    DEBUG_VALUE_ONLY(propertyName) << ", ";
#define DEBUG_VALUE_LAST(propertyName) \
    DEBUG_VALUE_ONLY(propertyName) << ")";
#define DEBUG_VALUE_INDENT(propertyName) \
    debug.nospace() << "\n    ";\
    DEBUG_VALUE(propertyName);
#define DEBUG_VALUE_INDENT_LAST(propertyName) \
    debug.nospace() << "\n    ";\
    DEBUG_VALUE_LAST(propertyName);

QDebug operator<<(QDebug debug, const GooglePeople::Source &value)
{
    debug.nospace() << "Source(";
    DEBUG_VALUE(type)
    DEBUG_VALUE_LAST(id);
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::FieldMetadata &value)
{
    debug.nospace() << "FieldMetadata(";
    DEBUG_VALUE(primary)
    DEBUG_VALUE(verified)
    DEBUG_VALUE_LAST(source)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::Address &value)
{
    debug.nospace() << "Address(";
    DEBUG_VALUE(metadata)
    DEBUG_VALUE(formattedValue)
    DEBUG_VALUE(type)
    DEBUG_VALUE(formattedType)
    DEBUG_VALUE(poBox)
    DEBUG_VALUE(streetAddress)
    DEBUG_VALUE(extendedAddress)
    DEBUG_VALUE(city)
    DEBUG_VALUE(region)
    DEBUG_VALUE(postalCode)
    DEBUG_VALUE(country)
    DEBUG_VALUE_LAST(countryCode)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::Biography &value)
{
    debug.nospace() << "Biography(";
    DEBUG_VALUE(metadata)
    DEBUG_VALUE(value)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::Birthday &value)
{
    debug.nospace() << "Birthday(";
    DEBUG_VALUE(metadata)
    DEBUG_VALUE(date)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::EmailAddress &value)
{
    debug.nospace() << "EmailAddress(";
    DEBUG_VALUE(metadata)
    DEBUG_VALUE(value)
    DEBUG_VALUE(type)
    DEBUG_VALUE(formattedType)
    DEBUG_VALUE_LAST(displayName)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::Event &value)
{
    debug.nospace() << "Event(";
    DEBUG_VALUE(metadata)
    DEBUG_VALUE(date)
    DEBUG_VALUE_LAST(type)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::Membership &value)
{
    debug.nospace() << "Membership(";
    DEBUG_VALUE(metadata)
    DEBUG_VALUE(contactGroupResourceName)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::Name &value)
{
    debug.nospace() << "Name(";
    DEBUG_VALUE(metadata)
    DEBUG_VALUE(familyName)
    DEBUG_VALUE(givenName)
    DEBUG_VALUE_LAST(middleName)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::Nickname &value)
{
    debug.nospace() << "Nickname(";
    DEBUG_VALUE(metadata)
    DEBUG_VALUE(value)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::Organization &value)
{
    debug.nospace() << "Organization(";
    DEBUG_VALUE(metadata)
    DEBUG_VALUE(name)
    DEBUG_VALUE(title)
    DEBUG_VALUE(jobDescription)
    DEBUG_VALUE_LAST(department)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::PhoneNumber &value)
{
    debug.nospace() << "PhoneNumber(";
    DEBUG_VALUE(metadata)
    DEBUG_VALUE(value)
    DEBUG_VALUE(type)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::PersonMetadata &value)
{
    debug.nospace() << "PersonMetadata(";
    DEBUG_VALUE(sources)
    DEBUG_VALUE(previousResourceNames)
    DEBUG_VALUE(linkedPeopleResourceNames)
    DEBUG_VALUE_LAST(deleted)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::Photo &value)
{
    debug.nospace() << "Photo(";
    DEBUG_VALUE(metadata)
    DEBUG_VALUE(url)
    DEBUG_VALUE_LAST(default_)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::Url &value)
{
    debug.nospace() << "Url(";
    DEBUG_VALUE(metadata)
    DEBUG_VALUE(value)
    DEBUG_VALUE(type)
    DEBUG_VALUE_LAST(formattedType)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::Person &value)
{
    debug.nospace() << "\nPerson(";
    DEBUG_VALUE_INDENT(resourceName)
    DEBUG_VALUE_INDENT(metadata)
    DEBUG_VALUE_INDENT(addresses)
    DEBUG_VALUE_INDENT(biographies)
    DEBUG_VALUE_INDENT(birthdays)
    DEBUG_VALUE_INDENT(emailAddresses)
    DEBUG_VALUE_INDENT(memberships)
    DEBUG_VALUE_INDENT(names)
    DEBUG_VALUE_INDENT(nicknames)
    DEBUG_VALUE_INDENT(organizations)
    DEBUG_VALUE_INDENT(phoneNumbers)
    DEBUG_VALUE_INDENT(photos)
    DEBUG_VALUE_INDENT_LAST(urls)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::ContactGroupMetadata &value)
{
    debug.nospace() << "ContactGroupMetadata(";
    DEBUG_VALUE(updateTime)
    DEBUG_VALUE_LAST(deleted)
    return debug.maybeSpace();
}

QDebug operator<<(QDebug debug, const GooglePeople::ContactGroup &value)
{
    debug.nospace() << "\nContactGroup(";
    DEBUG_VALUE_INDENT(resourceName)
    DEBUG_VALUE_INDENT(etag)
    DEBUG_VALUE_INDENT(contactGroupMetadata)
    DEBUG_VALUE_INDENT(groupType)
    DEBUG_VALUE_INDENT(name)
    DEBUG_VALUE_INDENT(formattedName)
    DEBUG_VALUE_INDENT(memberResourceNames)
    DEBUG_VALUE_INDENT_LAST(memberCount)
    return debug.maybeSpace();
}
