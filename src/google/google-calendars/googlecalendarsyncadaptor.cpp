/****************************************************************************
 **
 ** Copyright (C) 2013 - 2019 Jolla Ltd.
 ** Copyright (C) 2020 Open Mobile Platform LLC.
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

#include "googlecalendarsyncadaptor.h"
#include "googlecalendarincidencecomparator.h"
#include "trace.h"

#include <QtCore/QUrlQuery>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QSettings>
#include <QtCore/QSet>

#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>

#include <Accounts/Account>
#include <Accounts/Manager>
#include <Accounts/Service>

//----------------------------------------------

#define QDATEONLY_FORMAT    "yyyy-MM-dd"
#define RFC5545_FORMAT "yyyyMMddThhmmssZ"
#define RFC5545_FORMAT_NTZC "yyyyMMddThhmmss"
#define RFC5545_QDATE_FORMAT "yyyyMMdd"

namespace {

const int GOOGLE_CAL_SYNC_PLUGIN_VERSION = 3;
const QByteArray NOTEBOOK_SERVER_SYNC_TOKEN_PROPERTY = QByteArrayLiteral("syncToken");
const QByteArray NOTEBOOK_SERVER_ID_PROPERTY = QByteArrayLiteral("calendarServerId");
const QByteArray NOTEBOOK_EMAIL_PROPERTY = QByteArrayLiteral("userPrincipalEmail");
const QByteArray SERVER_COLOR_PROPERTY = QByteArrayLiteral("serverColor");
const int COLLISION_ERROR_MAX_CONSECUTIVE = 8;
const QByteArray VOLATILE_APP = QByteArrayLiteral("VOLATILE");
const QByteArray VOLATILE_NAME = QByteArrayLiteral("SYNC-FAILURE");
const QString ERROR_REASON_NON_ORGANIZER = QStringLiteral("forbiddenForNonOrganizer");
const QString ERROR_REASON_UPDATE_MIN_TOO_OLD = QStringLiteral("updatedMinTooLongAgo");

void errorDumpStr(const QString &str)
{
    // Dump the entire string to the log.
    // Note that the log cannot handle newlines,
    // so we separate the string into chunks.
    Q_FOREACH (const QString &chunk, str.split('\n', QString::SkipEmptyParts)) {
        qCWarning(lcSocialPlugin) << chunk;
    }
}

void traceDumpStr(const QString &str)
{
    if (!lcSocialPluginTrace().isDebugEnabled()) {
        return;
    }

    // Dump the entire string to the log.
    // Note that the log cannot handle newlines,
    // so we separate the string into chunks.
    Q_FOREACH (const QString &chunk, str.split('\n', QString::SkipEmptyParts)) {
        qCDebug(lcSocialPluginTrace) << chunk;
    }
}

// returns true if the ghost-event cleanup sync has been performed.
bool ghostEventCleanupPerformed()
{
    QString settingsFileName = QString::fromLatin1("%1/%2/gcal.ini")
            .arg(PRIVILEGED_DATA_DIR)
            .arg(QString::fromLatin1(SYNC_DATABASE_DIR));
    QSettings settingsFile(settingsFileName, QSettings::IniFormat);
    return settingsFile.value(QString::fromLatin1("cleaned"), QVariant::fromValue<bool>(false)).toBool();
}
void setGhostEventCleanupPerformed()
{
    QString settingsFileName = QString::fromLatin1("%1/%2/gcal.ini")
            .arg(PRIVILEGED_DATA_DIR)
            .arg(QString::fromLatin1(SYNC_DATABASE_DIR));
    QSettings settingsFile(settingsFileName, QSettings::IniFormat);
    settingsFile.setValue(QString::fromLatin1("cleaned"), QVariant::fromValue<bool>(true));
    settingsFile.sync();
}

void uniteIncidenceLists(const KCalendarCore::Incidence::List &first, KCalendarCore::Incidence::List *second)
{
    int originalSecondSize = second->size();
    bool foundMatch = false;
    Q_FOREACH (KCalendarCore::Incidence::Ptr inc, first) {
        foundMatch = false;
        for (int i = 0; i < originalSecondSize; ++i) {
            if (inc->uid() == second->at(i)->uid() && inc->recurrenceId() == second->at(i)->recurrenceId()) {
                // found a match
                foundMatch = true;
                break;
            }
        }
        if (!foundMatch) {
            second->append(inc);
        }
    }
}

QString gCalEventId(KCalendarCore::Incidence::Ptr event)
{
    // we abuse the comments field to store our gcal-id.
    // we should use a custom property, but those are deleted on incidence deletion.
    const QStringList &comments(event->comments());
    Q_FOREACH (const QString &comment, comments) {
        if (comment.startsWith("jolla-sociald:gcal-id:")) {
            return comment.mid(22);
        }
    }
    return QString();
}

void setGCalEventId(KCalendarCore::Incidence::Ptr event, const QString &id)
{
    // we abuse the comments field to store our gcal-id.
    // we should use a custom property, but those are deleted on incidence deletion.
    const QStringList &comments(event->comments());
    Q_FOREACH (const QString &comment, comments) {
        if (comment.startsWith("jolla-sociald:gcal-id:")) {
            // remove any existing gcal-id comment.
            if (!event->removeComment(comment)) {
                qCDebug(lcSocialPlugin) << "Unable to remove comment:" << comment;
            }
            break;
        }
    }
    event->addComment(QStringLiteral("jolla-sociald:gcal-id:%1").arg(id));
}

void setRemoteUidCustomField(KCalendarCore::Incidence::Ptr event, const QString &uid, const QString &id)
{
    // store it also in a custom property purely for invitation lookup purposes.
    if (!uid.isEmpty()) {
        event->setNonKDECustomProperty("X-SAILFISHOS-REMOTE-UID", uid.toUtf8());
    } else {
        // Google Calendar invites are sent as invitations with uid suffixed with @google.com.
        if (id.endsWith(QLatin1String("@google.com"), Qt::CaseInsensitive)) {
            event->setNonKDECustomProperty("X-SAILFISHOS-REMOTE-UID", id.toUtf8());
        } else {
            QString suffixedId = id;
            suffixedId.append(QLatin1String("@google.com"));
            event->setNonKDECustomProperty("X-SAILFISHOS-REMOTE-UID", suffixedId.toUtf8());
        }
    }
}

QString gCalETag(KCalendarCore::Incidence::Ptr event)
{
    return event->customProperty("jolla-sociald", "gcal-etag");
}

void setGCalETag(KCalendarCore::Incidence::Ptr event, const QString &etag)
{
    // note: custom properties are purged on incidence deletion.
    event->setCustomProperty("jolla-sociald", "gcal-etag", etag);
}

QList<QDateTime> datetimesFromExRDateStr(const QString &exrdatestr, bool *isDateOnly)
{
    // possible forms:
    // RDATE:19970714T123000Z
    // RDATE;VALUE=DATE-TIME:19970714T123000Z
    // RDATE;VALUE=DATE-TIME:19970714T123000Z,19970715T123000Z
    // RDATE;TZID=America/New_York:19970714T083000
    // RDATE;VALUE=PERIOD:19960403T020000Z/19960403T040000Z,19960404T010000Z/PT3H
    // RDATE;VALUE=DATE:19970101,19970120

    QList<QDateTime> retn;
    QString str = exrdatestr;
    *isDateOnly = false; // by default.

    if (str.startsWith(QStringLiteral("exdate"), Qt::CaseInsensitive)) {
        str.remove(0, 6);
    } else if (str.startsWith(QStringLiteral("rdate"), Qt::CaseInsensitive)) {
        str.remove(0, 5);
    } else {
        qCWarning(lcSocialPlugin) << "not an ex/rdate string:" << exrdatestr;
        return retn;
    }

    if (str.startsWith(';')) {
        str.remove(0,1);
        if (str.startsWith("VALUE=DATE-TIME:", Qt::CaseInsensitive)) {
            str.remove(0, 16);
            QStringList dts = str.split(',');
            Q_FOREACH (const QString &dtstr, dts) {
                if (dtstr.endsWith('Z')) {
                    // UTC
                    QDateTime dt = QDateTime::fromString(dtstr, RFC5545_FORMAT);
                    dt.setTimeSpec(Qt::UTC);
                    retn.append(dt);
                } else {
                    // Floating time
                    QDateTime dt = QDateTime::fromString(dtstr, RFC5545_FORMAT_NTZC);
                    dt.setTimeSpec(Qt::LocalTime);
                    retn.append(dt);
                }
            }
        } else if (str.startsWith("VALUE=DATE:", Qt::CaseInsensitive)) {
            str.remove(0, 11);
            QStringList dts = str.split(',');
            Q_FOREACH(const QString &dstr, dts) {
                QDate date = QLocale::c().toDate(dstr, RFC5545_QDATE_FORMAT);
                retn.append(QDateTime(date));
            }
        } else if (str.startsWith("VALUE=PERIOD:", Qt::CaseInsensitive)) {
            qCWarning(lcSocialPlugin) << "unsupported parameter in ex/rdate string:" << exrdatestr;
            // TODO: support PERIOD formats, or just switch to CalDAV for Google sync...
        } else if (str.startsWith("TZID=") && str.contains(':')) {
            str.remove(0, 5);
            QString tzidstr = str.mid(0, str.indexOf(':')); // something like: "Australia/Brisbane"
            QTimeZone tz(tzidstr.toUtf8());
            str.remove(0, tzidstr.size()+1);
            QStringList dts = str.split(',');
            Q_FOREACH (const QString &dtstr, dts) {
                QDateTime dt = QDateTime::fromString(dtstr, RFC5545_FORMAT_NTZC);
                if (!dt.isValid()) {
                    // try parsing from alternate formats
                    dt = QDateTime::fromString(dtstr, Qt::ISODate);
                }
                if (!dt.isValid()) {
                    qCWarning(lcSocialPlugin) << "unable to parse datetime from ex/rdate string:" << exrdatestr;
                } else {
                    if (tz.isValid()) {
                        dt.setTimeZone(tz);
                    } else {
                        dt.setTimeSpec(Qt::LocalTime);
                        qCInfo(lcSocialPlugin) << "WARNING: unknown tzid:" << tzidstr << "; assuming clock-time instead!";
                    }
                    retn.append(dt);
                }
            }
        } else {
            qCWarning(lcSocialPlugin) << "invalid parameter in ex/rdate string:" << exrdatestr;
        }
    } else if (str.startsWith(':')) {
        str.remove(0,1);
        QStringList dts = str.split(',');
        Q_FOREACH (const QString &dtstr, dts) {
            if (dtstr.endsWith('Z')) {
                // UTC
                QDateTime dt = QDateTime::fromString(dtstr, RFC5545_FORMAT);
                if (!dt.isValid()) {
                    // try parsing from alternate formats
                    dt = QDateTime::fromString(dtstr, Qt::ISODate);
                }
                if (!dt.isValid()) {
                    qCWarning(lcSocialPlugin) << "unable to parse datetime from ex/rdate string:" << exrdatestr;
                } else {
                    // parsed successfully
                    dt.setTimeSpec(Qt::UTC);
                    retn.append(dt);
                }
            } else {
                // Floating time
                QDateTime dt = QDateTime::fromString(dtstr, RFC5545_FORMAT_NTZC);
                if (!dt.isValid()) {
                    // try parsing from alternate formats
                    dt = QDateTime::fromString(dtstr, Qt::ISODate);
                }
                if (!dt.isValid()) {
                    qCWarning(lcSocialPlugin) << "unable to parse datetime from ex/rdate string:" << exrdatestr;
                } else {
                    // parsed successfully
                    dt.setTimeSpec(Qt::LocalTime);
                    retn.append(dt);
                }
            }
        }
    } else {
        qCWarning(lcSocialPlugin) << "not a valid ex/rdate string:" << exrdatestr;
    }

    return retn;
}

QJsonArray recurrenceArray(KCalendarCore::Event::Ptr event, KCalendarCore::ICalFormat &icalFormat, const QList<QDateTime> &exceptions)
{
    QJsonArray retn;

    // RRULE
    KCalendarCore::Recurrence *kcalRecurrence = event->recurrence();
    Q_FOREACH (KCalendarCore::RecurrenceRule *rrule, kcalRecurrence->rRules()) {
        QString rruleStr = icalFormat.toString(rrule);
        rruleStr.replace("\r\n", "");
        retn.append(QJsonValue(rruleStr));
    }

    // EXRULE
    Q_FOREACH (KCalendarCore::RecurrenceRule *exrule, kcalRecurrence->exRules()) {
        QString exruleStr = icalFormat.toString(exrule);
        exruleStr.replace("RRULE", "EXRULE");
        exruleStr.replace("\r\n", "");
        retn.append(QJsonValue(exruleStr));
    }

    // RDATE (date)
    QString rdates;
    Q_FOREACH (const QDate &rdate, kcalRecurrence->rDates()) {
        rdates.append(QLocale::c().toString(rdate, RFC5545_QDATE_FORMAT));
        rdates.append(',');
    }
    if (rdates.size()) {
        rdates.chop(1); // trailing comma
        retn.append(QJsonValue(QString::fromLatin1("RDATE;VALUE=DATE:%1").arg(rdates)));
    }

    // RDATE (date-time)
    QString rdatetimes;
    Q_FOREACH (const QDateTime &rdatetime, kcalRecurrence->rDateTimes()) {
        if (rdatetime.timeSpec() == Qt::LocalTime) {
            rdatetimes.append(rdatetime.toString(RFC5545_FORMAT_NTZC));
        } else {
            rdatetimes.append(rdatetime.toUTC().toString(RFC5545_FORMAT));
        }
        rdatetimes.append(',');
    }
    if (rdatetimes.size()) {
        rdatetimes.chop(1); // trailing comma
        retn.append(QJsonValue(QString::fromLatin1("RDATE;VALUE=DATE-TIME:%1").arg(rdatetimes)));
    }

    // EXDATE (date)
    QString exdates;
    Q_FOREACH (const QDate &exdate, kcalRecurrence->exDates()) {
        // mkcal adds an EXDATE for each exception event, whereas Google does not
        // So we only include the EXDATE if there's no exception associated with it
        if (!exceptions.contains(QDateTime(exdate))) {
            exdates.append(QLocale::c().toString(exdate, RFC5545_QDATE_FORMAT));
            exdates.append(',');
        }
    }
    if (exdates.size()) {
        exdates.chop(1); // trailing comma
        retn.append(QJsonValue(QString::fromLatin1("EXDATE;VALUE=DATE:%1").arg(exdates)));
    }

    // EXDATE (date-time)
    QString exdatetimes;
    Q_FOREACH (const QDateTime &exdatetime, kcalRecurrence->exDateTimes()) {
        // mkcal adds an EXDATE for each exception event, whereas Google does not
        // So we only include the EXDATE if there's no exception associated with it
        if (!exceptions.contains(exdatetime)) {
            if (exdatetime.timeSpec() == Qt::LocalTime) {
                exdatetimes.append(exdatetime.toString(RFC5545_FORMAT_NTZC));
            } else {
                exdatetimes.append(exdatetime.toUTC().toString(RFC5545_FORMAT));
            }
            exdatetimes.append(',');
        }
    }
    if (exdatetimes.size()) {
        exdatetimes.chop(1); // trailing comma
        retn.append(QJsonValue(QString::fromLatin1("EXDATE;VALUE=DATE-TIME:%1").arg(exdatetimes)));
    }

    return retn;
}

QDateTime parseRecurrenceId(const QJsonObject &originalStartTime)
{
    QString recurrenceIdStr;
    QString recurrenceIdTzStr;

    if (originalStartTime.contains(QLatin1String("date"))) {
        recurrenceIdStr = originalStartTime.value(QLatin1String("date")).toVariant().toString();
    } else {
        recurrenceIdStr = originalStartTime.value(QLatin1String("dateTime")).toVariant().toString();
        recurrenceIdTzStr = originalStartTime.value(QLatin1String("timeZone")).toVariant().toString();
    }

    QDateTime recurrenceId = QDateTime::fromString(recurrenceIdStr, Qt::ISODate);
    if (!recurrenceIdTzStr.isEmpty()) {
        recurrenceId = recurrenceId.toTimeZone(QTimeZone(recurrenceIdTzStr.toLatin1()));
    }
    return recurrenceId;
}

QDateTime parseDateTimeString(const QString &dateTimeStr)
{
    QDateTime parsedTime = QDateTime::fromString(dateTimeStr, Qt::ISODate);

    if (parsedTime.isNull()) {
        qWarning() << "Unable to parse date time from string:" << dateTimeStr;
        return QDateTime();
    }

    return parsedTime.toTimeZone(QTimeZone::systemTimeZone());
}

void extractCreatedAndUpdated(const QJsonObject &eventData,
                              QDateTime *created,
                              QDateTime *updated)
{
    const QString createdStr = eventData.value(QLatin1String("created")).toVariant().toString();
    const QString updatedStr = eventData.value(QLatin1String("updated")).toVariant().toString();

    if (!createdStr.isEmpty()) {
        *created = parseDateTimeString(createdStr);
    }

    if (!updatedStr.isEmpty()) {
        *updated = parseDateTimeString(updatedStr);
    }
}

void extractStartAndEnd(const QJsonObject &eventData,
                        bool *startExists,
                        bool *endExists,
                        bool *startIsDateOnly,
                        bool *endIsDateOnly,
                        bool *isAllDay,
                        QDateTime *start,
                        QDateTime *end)
{
    *startIsDateOnly = false, *endIsDateOnly = false;
    QString startTimeString, endTimeString;
    QJsonObject startTimeData = eventData.value(QLatin1String("start")).toObject();
    QJsonObject endTimeData = eventData.value(QLatin1String("end")).toObject();
    if (!startTimeData.value(QLatin1String("date")).toVariant().toString().isEmpty()) {
        *startExists = true;
        *startIsDateOnly = true; // all-day event.
        startTimeString = startTimeData.value(QLatin1String("date")).toVariant().toString();
    } else if (!startTimeData.value(QLatin1String("dateTime")).toVariant().toString().isEmpty()) {
        *startExists = true;
        startTimeString = startTimeData.value(QLatin1String("dateTime")).toVariant().toString();
    } else {
        *startExists = false;
    }
    if (!endTimeData.value(QLatin1String("date")).toVariant().toString().isEmpty()) {
        *endExists = true;
        *endIsDateOnly = true; // all-day event.
        endTimeString = endTimeData.value(QLatin1String("date")).toVariant().toString();
    } else if (!endTimeData.value(QLatin1String("dateTime")).toVariant().toString().isEmpty()) {
        *endExists = true;
        endTimeString = endTimeData.value(QLatin1String("dateTime")).toVariant().toString();
    } else {
        *endExists = false;
    }

    if (*startExists) {
        if (!*startIsDateOnly) {
            *start = parseDateTimeString(startTimeString);
        } else {
            *start = QDateTime(QLocale::c().toDate(startTimeString, QDATEONLY_FORMAT));
        }
    }

    if (*endExists) {
        if (!*endIsDateOnly) {
            *end = parseDateTimeString(endTimeString);
        } else {
            // Special handling for all-day events is required.
            if (*startExists && *startIsDateOnly) {
                if (QLocale::c().toDate(startTimeString, QDATEONLY_FORMAT)
                        == QLocale::c().toDate(endTimeString, QDATEONLY_FORMAT)) {
                    // single-day all-day event
                    *endExists = false;
                    *isAllDay = true;
                } else if (QLocale::c().toDate(startTimeString, QDATEONLY_FORMAT)
                        == QLocale::c().toDate(endTimeString, QDATEONLY_FORMAT).addDays(-1)) {
                    // Google will send a single-day all-day event has having an end-date
                    // of startDate+1 to conform to iCal spec.  Hence, this is actually
                    // a single-day all-day event, despite the difference in end-date.
                    *endExists = false;
                    *isAllDay = true;
                } else {
                    // multi-day all-day event.
                    // as noted above, Google will send all-day events as having an end-date
                    // of real-end-date+1 in order to conform to iCal spec (exclusive end dt).
                    *end = QDateTime(QLocale::c().toDate(endTimeString, QDATEONLY_FORMAT).addDays(-1));
                    *isAllDay = true;
                }
            } else {
                *end = QDateTime(QLocale::c().toDate(endTimeString, QDATEONLY_FORMAT));
                *isAllDay = false;
            }
        }
    }
}

void extractRecurrence(const QJsonArray &recurrence, KCalendarCore::Event::Ptr event, KCalendarCore::ICalFormat &icalFormat, const QList<QDateTime> &exceptions)
{
    KCalendarCore::Recurrence *kcalRecurrence = event->recurrence();
    kcalRecurrence->clear(); // avoid adding duplicate recurrence information
    for (int i = 0; i < recurrence.size(); ++i) {
        QString ruleStr = recurrence.at(i).toString();
        if (ruleStr.startsWith(QString::fromLatin1("rrule"), Qt::CaseInsensitive)) {
            KCalendarCore::RecurrenceRule *rrule = new KCalendarCore::RecurrenceRule;
            if (!icalFormat.fromString(rrule, ruleStr.mid(6))) {
                qCDebug(lcSocialPlugin) << "unable to parse RRULE information:" << ruleStr;
                traceDumpStr(QString::fromUtf8(QJsonDocument(recurrence).toJson()));
            } else {
                // Set the recurrence start to be the event start
                rrule->setStartDt(event->dtStart());
                kcalRecurrence->addRRule(rrule);
            }
        } else if (ruleStr.startsWith(QString::fromLatin1("exrule"), Qt::CaseInsensitive)) {
            KCalendarCore::RecurrenceRule *exrule = new KCalendarCore::RecurrenceRule;
            if (!icalFormat.fromString(exrule, ruleStr.mid(7))) {
                qCDebug(lcSocialPlugin) << "unable to parse EXRULE information:" << ruleStr;
                traceDumpStr(QString::fromUtf8(QJsonDocument(recurrence).toJson()));
            } else {
                kcalRecurrence->addExRule(exrule);
            }
        } else if (ruleStr.startsWith(QString::fromLatin1("rdate"), Qt::CaseInsensitive)) {
            bool isDateOnly = false;
            QList<QDateTime> rdatetimes = datetimesFromExRDateStr(ruleStr, &isDateOnly);
            if (!rdatetimes.size()) {
                qCDebug(lcSocialPlugin) << "unable to parse RDATE information:" << ruleStr;
                traceDumpStr(QString::fromUtf8(QJsonDocument(recurrence).toJson()));
            } else {
                Q_FOREACH (const QDateTime &dt, rdatetimes) {
                    if (isDateOnly) {
                        kcalRecurrence->addRDate(dt.date());
                    } else {
                        kcalRecurrence->addRDateTime(dt);
                    }
                }
            }
        } else if (ruleStr.startsWith(QString::fromLatin1("exdate"), Qt::CaseInsensitive)) {
            bool isDateOnly = false;
            QList<QDateTime> exdatetimes = datetimesFromExRDateStr(ruleStr, &isDateOnly);
            if (!exdatetimes.size()) {
                qCDebug(lcSocialPlugin) << "unable to parse EXDATE information:" << ruleStr;
                traceDumpStr(QString::fromUtf8(QJsonDocument(recurrence).toJson()));
            } else {
                Q_FOREACH (const QDateTime &dt, exdatetimes) {
                    if (isDateOnly) {
                        kcalRecurrence->addExDate(dt.date());
                    } else {
                        kcalRecurrence->addExDateTime(dt);
                    }
                }
            }
        } else {
          qCDebug(lcSocialPlugin) << "unknown recurrence information:" << ruleStr;
          traceDumpStr(QString::fromUtf8(QJsonDocument(recurrence).toJson()));
        }
    }

    // Add an extra EXDATE for each exception event the calendar
    // Google doesn't include these as EXDATE (following the spec) whereas mkcal does
    for (const QDateTime exception : exceptions) {
        if (exception.time().isNull()) {
            kcalRecurrence->addExDate(exception.date());
        } else {
            kcalRecurrence->addExDateTime(exception);
        }
    }
}

void extractOrganizer(const QJsonObject &creatorObj, const QJsonObject &organizerObj, KCalendarCore::Event::Ptr event)
{
    if (!organizerObj.value(QLatin1String("displayName")).toVariant().toString().isEmpty()
                    || !organizerObj.value(QLatin1String("email")).toVariant().toString().isEmpty()) {
            KCalendarCore::Person organizer(
                    organizerObj.value(QLatin1String("displayName")).toVariant().toString(),
                    organizerObj.value(QLatin1String("email")).toVariant().toString());
            event->setOrganizer(organizer);
    } else if (!creatorObj.value(QLatin1String("displayName")).toVariant().toString().isEmpty()
                || !creatorObj.value(QLatin1String("email")).toVariant().toString().isEmpty()) {
        KCalendarCore::Person organizer(
                creatorObj.value(QLatin1String("displayName")).toVariant().toString(),
                creatorObj.value(QLatin1String("email")).toVariant().toString());
        event->setOrganizer(organizer);
    }
}

void extractAttendees(const QJsonArray &attendees, KCalendarCore::Event::Ptr event)
{
    event->clearAttendees();
    for (int i = 0; i < attendees.size(); ++i) {
        QJsonObject attendeeObj = attendees.at(i).toObject();
        if (!attendeeObj.value(QLatin1String("organizer")).toVariant().toBool()) {
            KCalendarCore::Attendee attendee(
                    attendeeObj.value(QLatin1String("displayName")).toVariant().toString(),
                    attendeeObj.value(QLatin1String("email")).toVariant().toString());
            if (attendeeObj.find(QLatin1String("optional")) != attendeeObj.end()) {
                if (attendeeObj.value(QLatin1String("optional")).toVariant().toBool()) {
                    attendee.setRole(KCalendarCore::Attendee::OptParticipant);
                } else {
                    attendee.setRole(KCalendarCore::Attendee::ReqParticipant);
                }
            }
            if (attendeeObj.find(QLatin1String("responseStatus")) != attendeeObj.end()) {
                const QString &responseValue = attendeeObj.value(QLatin1String("responseStatus")).toVariant().toString();
                if (responseValue == "needsAction") {
                    attendee.setStatus(KCalendarCore::Attendee::NeedsAction);
                } else if (responseValue == "accepted") {
                    attendee.setStatus(KCalendarCore::Attendee::Accepted);
                } else if (responseValue == "declined") {
                    attendee.setStatus(KCalendarCore::Attendee::Declined);
                } else {
                    attendee.setStatus(KCalendarCore::Attendee::Tentative);
                }
            }
            attendee.setRSVP(true);
            event->addAttendee(attendee);
        } else {
            if (!event->organizer().isEmpty()) {
                continue;
            }
            if (!attendeeObj.value(QLatin1String("displayName")).toVariant().toString().isEmpty()
                        || !attendeeObj.value(QLatin1String("email")).toVariant().toString().isEmpty()) {
                KCalendarCore::Person organizer(
                        attendeeObj.value(QLatin1String("displayName")).toVariant().toString(),
                        attendeeObj.value(QLatin1String("email")).toVariant().toString());
                event->setOrganizer(organizer);
            }
        }
    }
}

#define START_EVENT_UPDATES_IF_REQUIRED(event, changed) \
    if (*changed == false) {                            \
        event->startUpdates();                          \
    }                                                   \
    *changed = true;

#define UPDATE_EVENT_PROPERTY_IF_REQUIRED(event, getter, setter, newValue, changed) \
    if (event->getter() != newValue) {                                              \
        START_EVENT_UPDATES_IF_REQUIRED(event, changed)                             \
        event->setter(newValue);                                                    \
    }

#define END_EVENT_UPDATES_IF_REQUIRED(event, changed, startedUpdates)               \
    if (*changed == false) {                                                        \
        qCDebug(lcSocialPlugin) << "Ignoring spurious change reported for:" <<       \
                          event->uid() << event->revision() << event->summary();    \
    } else if (startedUpdates) {                                                    \
        event->endUpdates();                                                        \
    }

void extractAlarms(const QJsonObject &json, KCalendarCore::Event::Ptr event, int defaultReminderStartOffset, bool *changed)
{
    QSet<int> startOffsets;
    if (json.contains(QStringLiteral("reminders"))) {
        QJsonObject reminders = json.value(QStringLiteral("reminders")).toObject();
        if (reminders.value(QStringLiteral("useDefault")).toBool()) {
            if (defaultReminderStartOffset > 0) {
                startOffsets.insert(defaultReminderStartOffset);
            } else {
                qCDebug(lcSocialPlugin) << "not adding default reminder even though requested: not popup or invalid start offset.";
            }
        } else {
            QJsonArray overrides = reminders.value(QStringLiteral("overrides")).toArray();
            for (int i = 0; i < overrides.size(); ++i) {
                QJsonObject override = overrides.at(i).toObject();
                if (override.value(QStringLiteral("method")).toString() == QStringLiteral("popup")) {
                    startOffsets.insert(override.value(QStringLiteral("minutes")).toInt());
                }
            }
        }
        // search for all reminders to see if they are represented by an alarm.
        bool needRemoveAndRecreate = false;
        for (QSet<int>::const_iterator it = startOffsets.constBegin(); it != startOffsets.constEnd(); it++) {
            const int startOffset = (*it) * -60; // convert minutes to seconds (before event)
            qCDebug(lcSocialPlugin) << "event needs reminder with start offset (seconds):" << startOffset;
            KCalendarCore::Alarm::List alarms = event->alarms();
            int alarmsCount = 0;
            for (int i = 0; i < alarms.count(); ++i) {
                // we don't count Procedure type alarms.
                if (alarms.at(i)->type() != KCalendarCore::Alarm::Procedure) {
                    alarmsCount += 1;
                    if (alarms.at(i)->startOffset().asSeconds() == startOffset) {
                        qCDebug(lcSocialPlugin) << "event already has reminder with start offset (seconds):" << startOffset;
                    } else {
                        qCDebug(lcSocialPlugin) << "event is missing reminder with start offset (seconds):" << startOffset;
                        needRemoveAndRecreate = true;
                    }
                }
            }
            if (alarmsCount != startOffsets.count()) {
                qCDebug(lcSocialPlugin) << "event has too many reminders, recreating alarms.";
                needRemoveAndRecreate = true;
            }
        }
        if (needRemoveAndRecreate) {
            START_EVENT_UPDATES_IF_REQUIRED(event, changed);
            KCalendarCore::Alarm::List alarms = event->alarms();
            for (int i = 0; i < alarms.count(); ++i) {
                if (alarms.at(i)->type() != KCalendarCore::Alarm::Procedure) {
                    event->removeAlarm(alarms.at(i));
                }
            }
            for (QSet<int>::const_iterator it = startOffsets.constBegin(); it != startOffsets.constEnd(); it++) {
                const int startOffset = (*it) * -60; // convert minutes to seconds (before event)
                qCDebug(lcSocialPlugin) << "setting event reminder with start offset (seconds):" << startOffset;
                KCalendarCore::Alarm::Ptr alarm = event->newAlarm();
                alarm->setEnabled(true);
                alarm->setStartOffset(KCalendarCore::Duration(startOffset));
            }
        }
    }
    if (startOffsets.isEmpty()) {
        // no reminders were defined in the json received from Google.
        // remove any alarms as required from the local event.
        KCalendarCore::Alarm::List alarms = event->alarms();
        for (int i = 0; i < alarms.count(); ++i) {
            if (alarms.at(i)->type() != KCalendarCore::Alarm::Procedure) {
                qCDebug(lcSocialPlugin) << "removing event reminder with start offset (seconds):" << alarms.at(i)->startOffset().asSeconds();
                START_EVENT_UPDATES_IF_REQUIRED(event, changed);
                event->removeAlarm(alarms.at(i));
            }
        }
    }
}

void jsonToKCal(const QJsonObject &json, KCalendarCore::Event::Ptr event, int defaultReminderStartOffset, KCalendarCore::ICalFormat &icalFormat, const QList<QDateTime> exceptions, bool *changed)
{
    Q_ASSERT(!event.isNull());
    bool alreadyStarted = *changed; // if this is true, we don't need to call startUpdates/endUpdates() in this function.
    const QString eventGCalETag(gCalETag(event));
    const QString jsonGCalETag(json.value(QLatin1String("etag")).toVariant().toString());
    if (!alreadyStarted && eventGCalETag == jsonGCalETag) {
        qCDebug(lcSocialPlugin) << "Ignoring non-remote-changed:" << event->uid() << ","
                          << eventGCalETag << "==" << jsonGCalETag;
        return; // this event has not changed server-side since we last saw it.
    }

    QDateTime createdTimestamp, updatedTimestamp, start, end;
    bool startExists = false, endExists = false;
    bool startIsDateOnly = false, endIsDateOnly = false;
    bool isAllDay = false;
    extractCreatedAndUpdated(json, &createdTimestamp, &updatedTimestamp);
    extractStartAndEnd(json, &startExists, &endExists, &startIsDateOnly, &endIsDateOnly, &isAllDay, &start, &end);
    if (gCalEventId(event) != json.value(QLatin1String("id")).toVariant().toString()) {
        START_EVENT_UPDATES_IF_REQUIRED(event, changed);
        setGCalEventId(event, json.value(QLatin1String("id")).toVariant().toString());
    }
    if (eventGCalETag != jsonGCalETag) {
        START_EVENT_UPDATES_IF_REQUIRED(event, changed);
        setGCalETag(event, jsonGCalETag);
    }
    setRemoteUidCustomField(event, json.value(QLatin1String("iCalUID")).toVariant().toString(), json.value(QLatin1String("id")).toVariant().toString());
    extractOrganizer(json.value(QLatin1String("creator")).toObject(), json.value(QLatin1String("organizer")).toObject(), event);
    extractAttendees(json.value(QLatin1String("attendees")).toArray(), event);
    UPDATE_EVENT_PROPERTY_IF_REQUIRED(event, isReadOnly, setReadOnly, json.value(QLatin1String("locked")).toVariant().toBool(), changed)
    UPDATE_EVENT_PROPERTY_IF_REQUIRED(event, summary, setSummary, json.value(QLatin1String("summary")).toVariant().toString(), changed)
    UPDATE_EVENT_PROPERTY_IF_REQUIRED(event, description, setDescription, json.value(QLatin1String("description")).toVariant().toString(), changed)
    UPDATE_EVENT_PROPERTY_IF_REQUIRED(event, location, setLocation, json.value(QLatin1String("location")).toVariant().toString(), changed)
    UPDATE_EVENT_PROPERTY_IF_REQUIRED(event, revision, setRevision, json.value(QLatin1String("sequence")).toVariant().toInt(), changed)
    if (createdTimestamp.isValid()) {
        UPDATE_EVENT_PROPERTY_IF_REQUIRED(event, created, setCreated, createdTimestamp, changed)
    }
    if (updatedTimestamp.isValid()) {
        UPDATE_EVENT_PROPERTY_IF_REQUIRED(event, lastModified, setLastModified, updatedTimestamp, changed)
    }
    if (startExists) {
        UPDATE_EVENT_PROPERTY_IF_REQUIRED(event, dtStart, setDtStart, start, changed)
    }
    if (endExists) {
        if (!event->hasEndDate() || event->dtEnd() != end) {
            START_EVENT_UPDATES_IF_REQUIRED(event, changed);
            event->setDtEnd(end);
        }
    }
    // Recurrence rules use the event start time, so must be set after it
    extractRecurrence(json.value(QLatin1String("recurrence")).toArray(), event, icalFormat, exceptions);
    if (isAllDay) {
        UPDATE_EVENT_PROPERTY_IF_REQUIRED(event, allDay, setAllDay, true, changed)
    }
    extractAlarms(json, event, defaultReminderStartOffset, changed);
    END_EVENT_UPDATES_IF_REQUIRED(event, changed, !alreadyStarted);
}

bool remoteModificationIsReal(const QJsonObject &json, KCalendarCore::Event::Ptr event)
{
    if (gCalEventId(event) != json.value(QLatin1String("id")).toVariant().toString()) {
        return true; // this event is either a partial-upsync-artifact or a new remote addition.
    }
    if (gCalETag(event) != json.value(QLatin1String("etag")).toVariant().toString()) {
        return true; // this event has changed server-side since we last saw it.
    }
    return false; // this event has not changed server-side since we last saw it.
}

bool localModificationIsReal(const QJsonObject &local, const QJsonObject &remote, int defaultReminderStartOffset, KCalendarCore::ICalFormat &icalFormat)
{
    bool changed = true;
    KCalendarCore::Event::Ptr localEvent = KCalendarCore::Event::Ptr(new KCalendarCore::Event);
    KCalendarCore::Event::Ptr remoteEvent = KCalendarCore::Event::Ptr(new KCalendarCore::Event);
    jsonToKCal(local, localEvent, defaultReminderStartOffset, icalFormat, QList<QDateTime>(), &changed);
    jsonToKCal(remote, remoteEvent, defaultReminderStartOffset, icalFormat, QList<QDateTime>(), &changed);
    if (GoogleCalendarIncidenceComparator::incidencesEqual(localEvent, remoteEvent)) {
        return false; // they're equal, so the local modification is not real.
    }
    return true;
}

// returns true if the last sync was marked as successful, and then marks the current
// sync as being unsuccessful.  The sync adapter should set it to true manually
// once sync succeeds.
bool wasLastSyncSuccessful(int accountId, bool *needCleanSync)
{
    QString settingsFileName = QString::fromLatin1("%1/%2/gcal.ini")
            .arg(PRIVILEGED_DATA_DIR)
            .arg(QString::fromLatin1(SYNC_DATABASE_DIR));
    if (!QFile::exists(settingsFileName)) {
        qCDebug(lcSocialPlugin) << "gcal.ini settings file does not exist, triggering clean sync";
        *needCleanSync = true;
        return false;
    }

    QSettings settingsFile(settingsFileName, QSettings::IniFormat);
    // needCleanSync will be true if and only if an unrecoverable error occurred during the previous sync.
    *needCleanSync = settingsFile.value(QString::fromLatin1("%1-needCleanSync").arg(accountId), QVariant::fromValue<bool>(false)).toBool();
    bool retn = settingsFile.value(QString::fromLatin1("%1-success").arg(accountId), QVariant::fromValue<bool>(false)).toBool();
    settingsFile.setValue(QString::fromLatin1("%1-success").arg(accountId), QVariant::fromValue<bool>(false));
    int pluginVersion = settingsFile.value(QString::fromLatin1("%1-pluginVersion").arg(accountId), QVariant::fromValue<int>(GOOGLE_CAL_SYNC_PLUGIN_VERSION)).toInt();
    if (pluginVersion != GOOGLE_CAL_SYNC_PLUGIN_VERSION) {
        qCDebug(lcSocialPlugin) << "Google cal sync plugin version mismatch, force clean sync";
        retn = false;
    }
    return retn;
}

void setLastSyncSuccessful(int accountId)
{
    QString settingsFileName = QString::fromLatin1("%1/%2/gcal.ini")
            .arg(PRIVILEGED_DATA_DIR)
            .arg(QString::fromLatin1(SYNC_DATABASE_DIR));
    QSettings settingsFile(settingsFileName, QSettings::IniFormat);
    settingsFile.setValue(QString::fromLatin1("%1-needCleanSync").arg(accountId), QVariant::fromValue<bool>(false));
    settingsFile.setValue(QString::fromLatin1("%1-success").arg(accountId), QVariant::fromValue<bool>(true));
    settingsFile.setValue(QString::fromLatin1("%1-pluginVersion").arg(accountId), GOOGLE_CAL_SYNC_PLUGIN_VERSION);
}

// Move all items with a recurrenceId after those without
// Retain the same order within the two groups
void reorderAdditions(KCalendarCore::Incidence::List &addedList) {
    // This is a QVector so we want to avoid expensive insertions/deletions
    int nextSwap = 0;
    for (int pos = 0; pos < addedList.size() - 1; ++pos) {
        const KCalendarCore::Incidence::Ptr from = addedList[pos];
        if (from->hasRecurrenceId()) {
            // Need to swap
            if (nextSwap <= pos) {
                nextSwap = pos + 1;
            }
            // Find the next sensible swap position
            while (nextSwap < addedList.size() - 1 && addedList[nextSwap]->hasRecurrenceId()) {
                ++nextSwap;
            }
            // Swap the item positions
            addedList[pos] = addedList[nextSwap];
            addedList[nextSwap] = from;
        }
    }
}

QString toBase32hex(QByteArray bytes)
{
    // See RFC2938 section 3.1.2
    static const char convert[] = "0123456789abcdefghijklmnopqrstuv";
    int const length = bytes.length();
    QString result;

    // pos represents the bit position, taking 5 at a time
    for (int pos = 0; pos < length * 8; pos += 5) {
        quint8 const startbyte = pos / 8;
        quint8 const underflowstart = pos % 8;
        quint8 const underflowbits = underflowstart > 3 ? 8 - underflowstart : 5;
        quint8 const overflowend = 5 - underflowbits;
        quint8 const underflowmask = ((2 << (underflowbits - 1)) - 1) << underflowstart;

        int val = (bytes[startbyte] & underflowmask) >> underflowstart;
        if ((overflowend > 0) && (startbyte < (length - 1))) {
            quint8 const overflowmask = ((2 << (overflowend - 1)) - 1);
            val += (bytes[startbyte + 1] & overflowmask) << underflowbits;
        }
        Q_ASSERT(val < 32);
        result += convert[val];
    }

    return result;
}

QString generate_uuid()
{
    // UUID documentation here:
    // https://developers.google.com/calendar/v3/reference/events#id
    // Generate using RFC4122
    return toBase32hex(QUuid::createUuid().toRfc4122());
}

QByteArray jsonReplaceValue(const QByteArray &json, const QString &key, const QJsonValue &value)
{
    // This is expensive, so should be used sparingly
    QJsonObject object = QJsonDocument::fromJson(json).object();
    object.insert(key, value);
    return QJsonDocument(object).toJson();
}

struct ErrorDetails {
    QString reason;
    QString message;
};

QString getErrorReason(const QByteArray &replyData)
{
    QString reason;
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(replyData, &error);
    if (error.error == QJsonParseError::NoError) {
        QJsonObject object = doc.object().value(QStringLiteral("error")).toObject();;
        reason = object.value(QStringLiteral("errors")).toArray().first().toObject().value(QStringLiteral("reason")).toString();
    } else {
        qCDebug(lcSocialPlugin) << "Json parse error:" << error.errorString();
    }

    return reason;
}

KCalendarCore::Event::Ptr dissociateSingleOccurrence(const KCalendarCore::Event::Ptr &event, const QDateTime &dateTime)
{
    // Create a new incidence
    KCalendarCore::Event::Ptr newEvent = KCalendarCore::Event::Ptr(event->clone());
    newEvent->setCreated(QDateTime::currentDateTimeUtc());
    newEvent->setSchedulingID(QString());
    if (newEvent->recurs()) {
        newEvent->clearRecurrence();
    }

    // Add a recurrence rule to the parent if it needs it
    // Add an ex-date to the parent for the dissociation
    if (event->allDay()) {
        if (!event->recursOn(dateTime.date(), dateTime.timeZone())) {
            event->recurrence()->addRDate(dateTime.date());
        }
        event->recurrence()->addExDate(dateTime.date());
    } else {
        if (!event->recursAt(dateTime)) {
            event->recurrence()->addRDateTime(dateTime);
        }
        event->recurrence()->addExDateTime(dateTime);
    }

    // Set the recurrenceId for the new incidence
    // Don't save milliseconds
    QDateTime recId(dateTime);
    recId.setTime(QTime(recId.time().hour(),
                        recId.time().minute(),
                        recId.time().second()));
    newEvent->setRecurrenceId(recId);

    return newEvent;
}

}

GoogleCalendarSyncAdaptor::GoogleCalendarSyncAdaptor(QObject *parent)
    : GoogleDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::Calendars, parent)
    , m_syncSucceeded(false)
    , m_accountId(0)
    , m_calendar(mKCal::ExtendedCalendar::Ptr(new mKCal::ExtendedCalendar(QTimeZone::utc())))
    , m_storage(mKCal::ExtendedCalendar::defaultStorage(m_calendar))
    , m_storageNeedsSave(false)
{
    m_calendar->setUpdateLastModifiedOnChange(false);
    setInitialActive(true);
}

GoogleCalendarSyncAdaptor::~GoogleCalendarSyncAdaptor()
{
}

QString GoogleCalendarSyncAdaptor::syncServiceName() const
{
    return QStringLiteral("google-calendars");
}

void GoogleCalendarSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    m_storage->open(); // we close it in finalCleanup()
    m_accountId = accountId; // needed by finalCleanup()
    GoogleDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void GoogleCalendarSyncAdaptor::finalCleanup()
{
    if (m_accountId != 0) {
        if (!m_syncSucceeded) {
            qCDebug(lcSocialPluginTrace) << "Sync failed, configuring for next sync";
            // sync failed.  check to see if we need to apply any changes to the database.
            const QSet<QString> calendarsRequiringChange = m_timeMinFailure + m_syncTokenFailure;
            const QDateTime yesterdayDate = QDateTime::currentDateTimeUtc().addDays(-1);
            for (const QString &calendarId : calendarsRequiringChange) {
                // this codepath is hit if the server replied with HTTP 410 for the sync token or timeMin value.
                if (mKCal::Notebook::Ptr notebook = notebookForCalendarId(calendarId)) {
                    if (m_syncTokenFailure.contains(calendarId)) {
                        // this sync cycle failed due to the sync token being invalidated server-side.
                        // trigger clean sync with wide time span on next sync.
                        qCInfo(lcSocialPlugin) << "Clearing sync time for calendar:" << calendarId;
                        notebook->setSyncDate(QDateTime());
                    } else if (m_timeMinFailure.contains(calendarId)) {
                        // this sync cycle failed due to the timeMin value being too far in the past.
                        // trigger clean sync with short time span on next sync.
                        qCInfo(lcSocialPlugin) << "Setting sync time to yesterday for calendar:" << calendarId;
                        notebook->setSyncDate(yesterdayDate);
                    }
                    notebook->setCustomProperty(NOTEBOOK_SERVER_SYNC_TOKEN_PROPERTY, QString());
                    m_storage->updateNotebook(notebook);
                    // Notebook operations are immediate so no need to amend m_storageNeedsSave
                }
            }
        } else {
            // sync succeeded.  apply the changes to the database.
            applyRemoteChangesLocally();
            if (!m_syncSucceeded) {
                qCInfo(lcSocialPlugin) << "Error occurred while applying remote changes locally";
            } else {
                Q_FOREACH (const QString &updatedCalendarId, m_calendarsFinishedRequested) {
                    // Update the sync date for the notebook, to the timestamp reported by Google
                    // in the calendar request for the remote calendar associated with the notebook,
                    // if that timestamp is recent (within the last week).  If it is older than that,
                    // update it to the current date minus one day, otherwise Google will return
                    // 410 GONE "UpdatedMin too old" error on subsequent requests.
                    mKCal::Notebook::Ptr notebook = notebookForCalendarId(updatedCalendarId);
                    if (!notebook) {
                        // may have been deleted due to a purge operation.
                        continue;
                    }

                    // Google doesn't use the sync date (synchronisation is handled by the token), it's
                    // only used by us to figure out what has changed since this sync, using either the
                    // lastModified or dateDeleted, both of which are set based on the client's time. We
                    // should therefore set the local synchronisation date to the client's time too.
                    // The "modified by" test inequality is inclusive, so changes from the sync have
                    // timestamp clamped to a second before the sync time using clampEventTimeToSync().
                    qCDebug(lcSocialPlugin) << "Latest sync date set to: " << m_syncedDateTime.toString();
                    notebook->setSyncDate(m_syncedDateTime);

                    // also update the remote sync token in each notebook.
                    notebook->setCustomProperty(NOTEBOOK_SERVER_SYNC_TOKEN_PROPERTY,
                                                m_calendarsNextSyncTokens.value(updatedCalendarId));
                    m_storage->updateNotebook(notebook);
                    // Notebook operations are immediate so no need to amend m_storageNeedsSave
                }
            }
        }

        // Flag any errors
        applySyncFailureFlags();
    }

    qCDebug(lcSocialPlugin) << "Saving:" << m_storageNeedsSave;
    if (m_storageNeedsSave) {
        m_storage->save(mKCal::ExtendedStorage::PurgeDeleted);
    }
    m_storageNeedsSave = false;

    if (!m_purgeList.isEmpty() && !m_storage->purgeDeletedIncidences(m_purgeList)) {
        // Silently ignore failed purge action in database.
        qCWarning(lcSocialPlugin) << "Cannot purge from database the marked as deleted incidences.";
    }

    // set the success status for each of our account settings.
    if (m_syncSucceeded) {
        setLastSyncSuccessful(m_accountId);
    }

    if (!ghostEventCleanupPerformed()) {
        // Delete any events which are not associated with a notebook.
        // These events are ghost events, caused by a bug which previously
        // existed in the sync adapter code due to mkcal deleteNotebook semantics.
        // The mkcal API doesn't allow us to determine which notebook a
        // given incidence belongs to, so we have to instead load
        // everything and then find the ones which are ophaned.
        // Note: we do this separately / after the commit above, because
        // loading all events from the database is expensive.
        qCInfo(lcSocialPlugin) << "performing ghost event cleanup";
        m_storage->load();
        KCalendarCore::Incidence::List allIncidences;
        m_storage->allIncidences(&allIncidences);
        mKCal::Notebook::List allNotebooks = m_storage->notebooks();
        QSet<QString> notebookIncidenceUids;
        foreach (mKCal::Notebook::Ptr notebook, allNotebooks) {
            KCalendarCore::Incidence::List currNbIncidences;
            m_storage->allIncidences(&currNbIncidences, notebook->uid());
            foreach (KCalendarCore::Incidence::Ptr incidence, currNbIncidences) {
                notebookIncidenceUids.insert(incidence->uid());
            }
        }
        int foundOrphans = 0;
        foreach (const KCalendarCore::Incidence::Ptr incidence, allIncidences) {
            if (!notebookIncidenceUids.contains(incidence->uid())) {
                // orphan/ghost incidence.  must be deleted.
                qCDebug(lcSocialPlugin) << "deleting local orphan event with uid:" << incidence->uid();
                m_calendar->deleteIncidence(m_calendar->incidence(incidence->uid(), incidence->recurrenceId()));
                foundOrphans++;
            }
        }
        if (foundOrphans == 0) {
            setGhostEventCleanupPerformed();
            qCInfo(lcSocialPlugin) << "orphan cleanup completed without finding orphans!";
        } else if (m_storage->save(mKCal::ExtendedStorage::PurgeDeleted)) {
            setGhostEventCleanupPerformed();
            qCInfo(lcSocialPlugin) << "orphan cleanup deleted" << foundOrphans << "; storage save completed!";
        } else {
            qCWarning(lcSocialPlugin) << "orphan cleanup found" << foundOrphans << "; but storage save failed!";
        }
    }

    m_storage->close();
    qCInfo(lcSocialPlugin) << "Sync completed";
}

void GoogleCalendarSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode mode)
{
    if (mode == SocialNetworkSyncAdaptor::CleanUpPurge) {
        // need to initialise the database
        m_storage->open(); // we close it in finalCleanup()
    }

    // We clean all the entries in the calendar
    // Delete the notebooks from the storage
    foreach (mKCal::Notebook::Ptr notebook, m_storage->notebooks()) {
        if (notebook->pluginName().startsWith(QStringLiteral("google"))
                && notebook->account() == QString::number(oldId)) {
            // remove the incidences and delete the notebook
            notebook->setIsReadOnly(false);
            m_storage->deleteNotebook(notebook);
            m_storageNeedsSave = true;
        }
    }

    if (mode == SocialNetworkSyncAdaptor::CleanUpPurge) {
        // and commit any changes made.
        finalCleanup();
    }
}

void GoogleCalendarSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    qCDebug(lcSocialPlugin) << "beginning Calendar sync for Google, account" << accountId;
    Q_ASSERT(accountId == m_accountId);
    bool needCleanSync = false;
    bool lastSyncSuccessful = wasLastSyncSuccessful(accountId, &needCleanSync);
    if (needCleanSync) {
        qCInfo(lcSocialPlugin) << "performing clean sync";
    } else if (!lastSyncSuccessful) {
        qCInfo(lcSocialPlugin) << "last sync was not successful, attempting to recover without clean sync";
    }
    m_serverCalendarIdToCalendarInfo.clear();
    m_calendarIdToEventObjects.clear();
    m_purgeList.clear();
    m_deletedGcalIdToIncidence.clear();
    m_sequenced.clear();
    m_eventSyncFlags.clear();
    m_syncSucceeded = true; // set to false on error
    m_syncedDateTime = QDateTime::currentDateTimeUtc();
    m_collisionErrorCount = 0;
    requestCalendars(accessToken, needCleanSync);
}

void GoogleCalendarSyncAdaptor::requestCalendars(const QString &accessToken, bool needCleanSync, const QString &pageToken)
{
    QList<QPair<QString, QString> > queryItems;
    if (!pageToken.isEmpty()) { // continuation request
        queryItems.append(QPair<QString, QString>(QString::fromLatin1("pageToken"),
                                                  pageToken));
    }

    QUrl url(QLatin1String("https://www.googleapis.com/calendar/v3/users/me/calendarList"));
    QUrlQuery query(url);
    query.setQueryItems(queryItems);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("GData-Version", "3.0");
    request.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                         QString(QLatin1String("Bearer ") + accessToken).toUtf8());

    QNetworkReply *reply = m_networkAccessManager->get(request);

    // we're requesting data.  Increment the semaphore so that we know we're still busy.
    incrementSemaphore(m_accountId);

    if (reply) {
        reply->setProperty("accountId", m_accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("needCleanSync", QVariant::fromValue<bool>(needCleanSync));
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)),
                this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)),
                this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(calendarsFinishedHandler()));

        setupReplyTimeout(m_accountId, reply);
    } else {
        qCWarning(lcSocialPlugin) << "unable to request calendars from Google account with id" << m_accountId;
        m_syncSucceeded = false;
        decrementSemaphore(m_accountId);
    }
}

void GoogleCalendarSyncAdaptor::calendarsFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    Q_ASSERT(reply->property("accountId").toInt() == m_accountId);
    QString accessToken = reply->property("accessToken").toString();
    bool needCleanSync = reply->property("needCleanSync").toBool();
    QByteArray replyData = reply->readAll();
    bool isError = reply->property("isError").toBool();

    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(m_accountId, reply);

    // parse the calendars' metadata from the response.
    bool fetchingNextPage = false;
    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);
    if (!isError && ok) {
        // first, check to see if there are more pages of calendars to fetch
        if (parsed.find(QLatin1String("nextPageToken")) != parsed.end()
                && !parsed.value(QLatin1String("nextPageToken")).toVariant().toString().isEmpty()) {
            fetchingNextPage = true;
            requestCalendars(accessToken, needCleanSync,
                             parsed.value(QLatin1String("nextPageToken")).toVariant().toString());
        }

        // second, parse the calendars' metadata
        QJsonArray items = parsed.value(QStringLiteral("items")).toArray();
        for (int i = 0; i < items.count(); ++i) {
            QJsonObject currCalendar = items.at(i).toObject();
            if (!currCalendar.isEmpty() && currCalendar.find(QStringLiteral("id")) != currCalendar.end()) {
                // we only sync calendars which the user owns (ie, not autogenerated calendars)
                QString accessRole = currCalendar.value(QStringLiteral("accessRole")).toString();
                if (accessRole == QStringLiteral("owner") || accessRole == QStringLiteral("writer")) {
                    GoogleCalendarSyncAdaptor::CalendarInfo currCalendarInfo;
                    currCalendarInfo.color = currCalendar.value(QStringLiteral("backgroundColor")).toString();
                    currCalendarInfo.summary = currCalendar.value(QStringLiteral("summary")).toString();
                    currCalendarInfo.description = currCalendar.value(QStringLiteral("description")).toString();
                    currCalendarInfo.change = NoChange; // we detect the appropriate change type (if required) later.
                    if (accessRole == QStringLiteral("owner")) {
                        currCalendarInfo.access = Owner;
                    } else {
                        currCalendarInfo.access = Writer;
                    }
                    QString currCalendarId = currCalendar.value(QStringLiteral("id")).toString();
                    m_serverCalendarIdToCalendarInfo.insert(currCalendarId, currCalendarInfo);
                }
            }
        }
    } else {
        // error occurred during request.
        qCWarning(lcSocialPlugin) << "unable to parse calendar data from request with account" << m_accountId << "; got:";
        errorDumpStr(QString::fromLatin1(replyData.constData()));
        m_syncSucceeded = false;
    }

    if (!fetchingNextPage) {
        // we've finished loading all pages of calendar information
        // we now need to process the loaded information to determine
        // which calendars need to be added/updated/removed locally.
        updateLocalCalendarNotebooks(accessToken, needCleanSync);
    }

    // we're finished with this request.
    decrementSemaphore(m_accountId);
}

void GoogleCalendarSyncAdaptor::updateLocalCalendarNotebooks(const QString &accessToken, bool needCleanSync)
{
    if (syncAborted()) {
        qCDebug(lcSocialPlugin) << "sync aborted, skipping updating local calendar notebooks";
        return;
    }

    QMap<QString, CalendarInfo> &calendars = m_serverCalendarIdToCalendarInfo;
    QMap<QString, QString> serverCalendarIdToSyncToken;

    // any calendars which exist on the device but not the server need to be purged.
    QStringList calendarsToDelete;
    QStringList deviceCalendarIds;
    foreach (mKCal::Notebook::Ptr notebook, m_storage->notebooks()) {
        if (notebook->pluginName().startsWith(QStringLiteral("google"))
                && notebook->account() == QString::number(m_accountId)) {
            // back compat: notebook pluginName used to be of form: google-calendarId
            const QString currDeviceCalendarId = notebook->pluginName().startsWith(QStringLiteral("google-"))
                                               ? notebook->pluginName().mid(7)
                                               : notebook->customProperty(NOTEBOOK_SERVER_ID_PROPERTY);
            if (calendars.contains(currDeviceCalendarId)) {
                // the server-side calendar exists on the device.
                const QString notebookNextSyncToken = notebook->customProperty(NOTEBOOK_SERVER_SYNC_TOKEN_PROPERTY);
                if (!notebookNextSyncToken.isEmpty()) {
                    serverCalendarIdToSyncToken.insert(currDeviceCalendarId, notebookNextSyncToken);
                }

                // check to see if we need to perform a clean sync cycle with this notebook.
                // if the sync token is empty and the syncTime is invalid, perform a clean sync anyway
                const bool effectiveCleanSync = (notebookNextSyncToken.isEmpty()
                                                 && (!notebook->syncDate().isValid()
                                                     || notebook->syncDate().toMSecsSinceEpoch() == 0));
                if (needCleanSync || effectiveCleanSync) {
                    // we are performing a clean sync cycle.
                    // we will eventually delete and then insert this notebook.
                    qCDebug(lcSocialPlugin) << "queueing clean sync of local calendar" << notebook->name()
                                      << currDeviceCalendarId << "for Google account:" << m_accountId;
                    deviceCalendarIds.append(currDeviceCalendarId);
                    calendars[currDeviceCalendarId].change = GoogleCalendarSyncAdaptor::CleanSync;
                } else {
                    // we don't need to purge it, but we may need to update its summary/color details.
                    deviceCalendarIds.append(currDeviceCalendarId);
                    if (notebook->name() != calendars.value(currDeviceCalendarId).summary
                            || notebook->color() != calendars.value(currDeviceCalendarId).color
                            || notebook->description() != calendars.value(currDeviceCalendarId).description
                            || notebook->sharedWith() != QStringList(currDeviceCalendarId)
                            || notebook->isReadOnly()) {
                        // calendar information changed server-side.
                        qCDebug(lcSocialPlugin) << "queueing modification of local calendar" << notebook->name()
                                          << currDeviceCalendarId << "for Google account:" << m_accountId;
                        calendars[currDeviceCalendarId].change = GoogleCalendarSyncAdaptor::Modify;
                    } else {
                        // the calendar information is unchanged server-side.
                        // no need to change anything locally.
                        qCDebug(lcSocialPlugin) << "No modification required for local calendar" << notebook->name()
                                          << currDeviceCalendarId << "for Google account:" << m_accountId;
                        calendars[currDeviceCalendarId].change = GoogleCalendarSyncAdaptor::NoChange;
                    }
                }
            } else {
                // the calendar has been removed from the server.
                // we need to purge it from the device.
                qCDebug(lcSocialPlugin) << "queueing removal of local calendar" << notebook->name() << currDeviceCalendarId
                                  << "for Google account:" << m_accountId;
                calendarsToDelete.append(currDeviceCalendarId);
            }
        }
    }

    // any calendarIds which exist on the server but not the device need to be created.
    foreach (const QString &serverCalendarId, calendars.keys()) {
        if (!deviceCalendarIds.contains(serverCalendarId)) {
            qCDebug(lcSocialPlugin) << "queueing addition of local calendar" << serverCalendarId
                              << calendars.value(serverCalendarId).summary
                              << "for Google account:" << m_accountId;
            calendars[serverCalendarId].change = GoogleCalendarSyncAdaptor::Insert;
        }
    }

    qCDebug(lcSocialPlugin) << "Syncing calendar events for Google account: " << m_accountId << " CleanSync: " << needCleanSync;

    foreach (const QString &calendarId, calendars.keys()) {
        const QString syncToken = isCleanSync(calendarId) ? QString() : serverCalendarIdToSyncToken.value(calendarId);
        requestEvents(accessToken, calendarId, syncToken);
        m_calendarsBeingRequested.append(calendarId);
    }

    // now we can queue the calendars which need deletion.
    // note: we have to do it after the previous foreach loop, otherwise we'd attempt to retrieve events for them.
    foreach (const QString &currDeviceCalendarId, calendarsToDelete) {
        calendars[currDeviceCalendarId].change = GoogleCalendarSyncAdaptor::Delete;
    }
}

void GoogleCalendarSyncAdaptor::requestEvents(const QString &accessToken, const QString &calendarId,
                                              const QString &syncToken, const QString &pageToken)
{
    mKCal::Notebook::Ptr notebook = notebookForCalendarId(calendarId);

    // get the last sync date stored into the notebook (if it exists).
    // we need to perform a "clean" sync if we don't have a valid sync date
    // or if we don't have a valid syncToken.

    // The mKCal API doesn't provide a way to get all deleted/modified incidences
    // for a notebook, as it implements the SQL query using an inequality on both modifiedAfter
    // and createdBefore; so instead we have to build a datetime which "should" satisfy
    // the inequality for all possible local modifications detectable since the last sync.
    QDateTime syncDate = notebook ? notebook->syncDate().addSecs(1) : QDateTime();
    const bool needCleanSync = isCleanSync(calendarId);

    if (!needCleanSync) {
        qCDebug(lcSocialPlugin) << "Previous sync time for Google account:" << m_accountId <<
                          "Calendar Id:" << calendarId <<
                          "- Times:" << syncDate.toString() <<
                          "- SyncToken:" << syncToken;
    } else if (syncDate.isValid() && syncToken.isEmpty()) {
        qCDebug(lcSocialPlugin) << "Clean sync required for Google account:" << m_accountId <<
                          "Calendar Id:" << calendarId <<
                          "- Ignoring last sync time:" << syncDate.toString();
        syncDate = QDateTime();
    } else {
        qCDebug(lcSocialPlugin) << "Invalid previous sync time for Google account:" << m_accountId <<
                          "Calendar Id:" << calendarId <<
                          "- Time:" << syncDate.toString() <<
                          "- SyncToken:" << syncToken;
    }

    QList<QPair<QString, QString> > queryItems;
    if (!needCleanSync) { // delta update request
        queryItems.append(QPair<QString, QString>(QString::fromLatin1("syncToken"), syncToken));
    } else { // clean sync request
        // Note: if the syncDate is valid, that should be because we previously
        // suffered from a 410 error due to the timeMin value being too long ago,
        // and we detected that case and wrote the next sync date value to use here.
        const QDateTime clampMin = QDateTime::currentDateTimeUtc().addYears(-1);
        const QDateTime timeMax = QDateTime::currentDateTimeUtc().addYears(2);
        syncDate = (!syncDate.isValid() || (syncDate < clampMin)) ? clampMin : syncDate;

        queryItems.append(QPair<QString, QString>(QString::fromLatin1("timeMin"), syncDate.toString(Qt::ISODate)));
        queryItems.append(QPair<QString, QString>(QString::fromLatin1("timeMax"), timeMax.toString(Qt::ISODate)));
    }
    if (!pageToken.isEmpty()) { // continuation request
        queryItems.append(QPair<QString, QString>(QString::fromLatin1("pageToken"), pageToken));
    }

    QUrl url(QString::fromLatin1("https://www.googleapis.com/calendar/v3/calendars/%1/events").arg(calendarId));
    QUrlQuery query(url);
    query.setQueryItems(queryItems);
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setRawHeader("GData-Version", "3.0");
    request.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                         QString(QLatin1String("Bearer ") + accessToken).toUtf8());

    QNetworkReply *reply = m_networkAccessManager->get(request);

    // we're requesting data.  Increment the semaphore so that we know we're still busy.
    incrementSemaphore(m_accountId);

    if (reply) {
        reply->setProperty("accountId", m_accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("calendarId", calendarId);
        reply->setProperty("syncToken", needCleanSync ? QString() : syncToken);
        reply->setProperty("since", syncDate);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)),
                this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)),
                this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(eventsFinishedHandler()));

        qCDebug(lcSocialPlugin) << "requesting calendar events for Google account:" << m_accountId << ":" << url.toString();

        setupReplyTimeout(m_accountId, reply);
    } else {
        qCWarning(lcSocialPlugin) << "unable to request events for calendar" << calendarId <<
                          "from Google account with id" << m_accountId;
        m_syncSucceeded = false;
        decrementSemaphore(m_accountId);
    }
}

void GoogleCalendarSyncAdaptor::eventsFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    Q_ASSERT(reply->property("accountId").toInt() == m_accountId);
    QString calendarId = reply->property("calendarId").toString();
    QString accessToken = reply->property("accessToken").toString();
    QString syncToken = reply->property("syncToken").toString();
    QDateTime since = reply->property("since").toDateTime();
    QByteArray replyData = reply->readAll();
    bool isError = reply->property("isError").toBool();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    QString replyString = QString::fromUtf8(replyData);
    qCDebug(lcSocialPluginTrace) << "-------------------------------";
    qCDebug(lcSocialPluginTrace) << "Events response for calendar:" << calendarId << "from account:" << m_accountId;
    qCDebug(lcSocialPluginTrace) << "HTTP CODE:" << httpCode;
    Q_FOREACH (QString line, replyString.split('\n', QString::SkipEmptyParts)) {
        qCDebug(lcSocialPluginTrace) << line.replace('\r', ' ');
    }
    qCDebug(lcSocialPluginTrace) << "-------------------------------";

    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(m_accountId, reply);

    bool fetchingNextPage = false;
    bool ok = false;
    QString nextSyncToken;
    const QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);
    if (!isError && ok) {
        // If there are more pages of results to fetch, ensure we fetch them
        if (parsed.find(QLatin1String("nextPageToken")) != parsed.end()
                && !parsed.value(QLatin1String("nextPageToken")).toVariant().toString().isEmpty()) {
            fetchingNextPage = true;
            requestEvents(accessToken, calendarId, syncToken,
                          parsed.value(QLatin1String("nextPageToken")).toVariant().toString());
        }

        // Otherwise, if we get a new sync token, ensure we store that for next sync
        nextSyncToken = parsed.value(QLatin1String("nextSyncToken")).toVariant().toString();

        // parse the default reminders data to find the default popup reminder start offset.
        if (parsed.find(QStringLiteral("defaultReminders")) != parsed.end()) {
            const QJsonArray defaultReminders = parsed.value(QStringLiteral("defaultReminders")).toArray();
            for (int i = 0; i < defaultReminders.size(); ++i) {
                QJsonObject defaultReminder = defaultReminders.at(i).toObject();
                if (defaultReminder.value(QStringLiteral("method")).toString() == QStringLiteral("popup")) {
                    m_serverCalendarIdToDefaultReminderTimes[calendarId] = defaultReminder.value(QStringLiteral("minutes")).toInt();
                }
            }
        }

        // Parse the event list
        const QJsonArray dataList = parsed.value(QLatin1String("items")).toArray();

        foreach (const QJsonValue &item, dataList) {
            QJsonObject eventData = item.toObject();

            // otherwise, we queue the event for insertion into the database.
            m_calendarIdToEventObjects.insertMulti(calendarId, eventData);
        }
    } else {
        // error occurred during request.
        if (httpCode == 410) {
            // HTTP 410 GONE is emitted if the syncToken or timeMin parameters are invalid.
            // We should trigger a clean sync with this notebook if we hit this error.
            const QString reason = getErrorReason(replyData);
            qCWarning(lcSocialPlugin) << "received 410 GONE" << reason << "from server; marking calendar" << calendarId << "from account" << m_accountId << "for clean sync";
            nextSyncToken.clear();
            if (reason == ERROR_REASON_UPDATE_MIN_TOO_OLD) {
                m_timeMinFailure.insert(calendarId);
            } else {
                m_syncTokenFailure.insert(calendarId);
            }
        } else {
            qCWarning(lcSocialPlugin) << "unable to parse event data from request with account" << m_accountId << "; got:";
            errorDumpStr(QString::fromUtf8(replyData.constData()));
        }
        m_syncSucceeded = false;
    }

    if (!fetchingNextPage) {
        // we've finished loading all pages of event information
        // we now need to process the loaded information to determine
        // which events need to be added/updated/removed locally.
        finishedRequestingRemoteEvents(accessToken, calendarId, syncToken, nextSyncToken, since);
    }

    // we're finished this request.  Decrement our busy semaphore.
    decrementSemaphore(m_accountId);
}

mKCal::Notebook::Ptr GoogleCalendarSyncAdaptor::notebookForCalendarId(const QString &calendarId) const
{
    foreach (mKCal::Notebook::Ptr notebook, m_storage->notebooks()) {
        if (notebook->account() == QString::number(m_accountId)
                && (notebook->customProperty(NOTEBOOK_SERVER_ID_PROPERTY) == calendarId
                       // for backward compatibility with old accounts / notebooks:
                    || notebook->pluginName() == QString::fromLatin1("google-%1").arg(calendarId))) {
            return notebook;
        }
    }

    return mKCal::Notebook::Ptr();
}

void GoogleCalendarSyncAdaptor::finishedRequestingRemoteEvents(const QString &accessToken,
                                                               const QString &calendarId, const QString &syncToken,
                                                               const QString &nextSyncToken, const QDateTime &since)
{
    m_calendarsBeingRequested.removeAll(calendarId);
    m_calendarsFinishedRequested.append(calendarId);
    m_calendarsThisSyncTokens.insert(calendarId, syncToken);
    m_calendarsNextSyncTokens.insert(calendarId, nextSyncToken);
    m_calendarsSyncDate.insert(calendarId, since);
    if (!m_calendarsBeingRequested.isEmpty()) {
        return; // still waiting for more requests to finish.
    }

    if (syncAborted() || !m_syncSucceeded) {
        return; // sync was aborted or failed before we received all remote data, and before we could upsync local changes.
    }

    // We're about to perform the delta, so record the time to use for the next sync
    m_syncedDateTime = QDateTime::currentDateTimeUtc();

    // determine local changes to upsync.
    Q_FOREACH (const QString &finishedCalendarId, m_calendarsFinishedRequested) {
        // now upsync the local changes to the remote server
        QList<UpsyncChange> changesToUpsync = determineSyncDelta(accessToken, finishedCalendarId, m_calendarsSyncDate.value(finishedCalendarId));
        if (changesToUpsync.size()) {
            if (syncAborted()) {
                qCDebug(lcSocialPlugin) << "skipping upsync of queued upsync changes due to sync being aborted";
            } else if (m_syncSucceeded == false) {
                qCDebug(lcSocialPlugin) << "skipping upsync of queued upsync changes due to previous error during sync";
            } else {
                qCDebug(lcSocialPlugin) << "upsyncing" << changesToUpsync.size() << "local changes to the remote server";
                for (int i = 0; i < changesToUpsync.size(); ++i) {
                    upsyncChanges(changesToUpsync[i]);
                }
            }
        } else {
            // no local changes to upsync.
            // we can apply the remote changes and we are finished.
        }
    }
}

// Return a list of all dates in the recurrence pattern that have an exception event associated with them
// Events must be loaded into memeory first before calling this method
const QList<QDateTime> GoogleCalendarSyncAdaptor::getExceptionInstanceDates(const KCalendarCore::Event::Ptr event) const
{
    QList<QDateTime> exceptions;

    // Get all the instances of the event
    KCalendarCore::Incidence::List instances = m_calendar->instances(event);
    for (const KCalendarCore::Incidence::Ptr incidence : instances) {
        if (incidence->hasRecurrenceId()) {
            // Record its recurrence Id
            exceptions += incidence->recurrenceId();
        }
    }

    return exceptions;
}

// Determine the sync delta, and then cache the required downsynced changes and return the required changes to upsync.
QList<GoogleCalendarSyncAdaptor::UpsyncChange> GoogleCalendarSyncAdaptor::determineSyncDelta(const QString &accessToken,
                                                                                             const QString &calendarId,
                                                                                             const QDateTime &since)
{
    Q_UNUSED(accessToken) // in the future, we might need it to download images/data associated with the event.

    QList<UpsyncChange> changesToUpsync;

    // Search for the device Notebook matching this CalendarId.
    // Only upsync changes if we're doing a delta sync, and upsync is enabled.
    bool upsyncEnabled = true;
    mKCal::Notebook::Ptr googleNotebook = notebookForCalendarId(calendarId);
    if (googleNotebook.isNull()) {
        // this is a new, never before seen calendar.
        qCInfo(lcSocialPlugin) << "No local calendar exists for:" << calendarId <<
                         "for account:" << m_accountId << ".  No upsync possible.";
        upsyncEnabled = false;
    } else if (!m_accountSyncProfile || m_accountSyncProfile->syncDirection() == Buteo::SyncProfile::SYNC_DIRECTION_FROM_REMOTE) {
        qCInfo(lcSocialPlugin) << "skipping upload of local calendar changes to" << calendarId <<
                         "due to profile direction setting for account" << m_accountId;
        upsyncEnabled = false;
    } else if (!since.isValid()) {
        qCInfo(lcSocialPlugin) << "Delta upsync with Google calendar" << calendarId <<
                         "for account" << m_accountId << "not required due to clean sync";
        upsyncEnabled = false;
    } else {
        qCInfo(lcSocialPlugin) << "Delta upsync with Google calendar" << calendarId <<
                         "for account" << m_accountId << "is enabled.";
        upsyncEnabled = true;
    }


    // re-order the list of remote events so that base recurring events will precede occurrences.
    QList<QJsonObject> eventObjects;
    foreach (const QJsonObject &eventData, m_calendarIdToEventObjects.values(calendarId)) {
        if (eventData.value(QLatin1String("recurringEventId")).toVariant().toString().isEmpty()) {
            // base event; prepend to list.
            eventObjects.prepend(eventData);
        } else {
            // occurrence; append to list.
            eventObjects.append(eventData);
        }
    }

    // parse that list to look for partial-upsync-artifacts.
    // if we upsynced some local addition, and then lost connectivity,
    // the remote server-side will report that as a remote addition upon
    // the next sync - but really it isn't.
    QHash<QString, QString> upsyncedUidMapping;
    QSet<QString> partialUpsyncArtifactsNeedingUpdate; // set of gcalIds
    foreach (const QJsonObject &eventData, eventObjects) {
        const QString eventId = eventData.value(QLatin1String("id")).toVariant().toString();
        const QString upsyncedUid = eventData.value(QLatin1String("extendedProperties")).toObject()
                                             .value(QLatin1String("private")).toObject()
                                             .value(QLatin1String("x-jolla-sociald-mkcal-uid")).toVariant().toString();
        if (!upsyncedUid.isEmpty() && !eventId.isEmpty()) {
            upsyncedUidMapping.insert(upsyncedUid, eventId);
        }
    }

    // load local events from the database.
    KCalendarCore::Incidence::List deletedList, extraDeletedList, addedList, updatedList, allList;
    QMap<QString, KCalendarCore::Event::Ptr> allMap, updatedMap;
    QMap<QString, QPair<QString, QDateTime> > deletedMap; // gcalId to incidenceUid,recurrenceId
    QSet<QString> cleanSyncDeletionAdditions; // gcalIds

    if (!isCleanSync(calendarId) && !googleNotebook.isNull()) {
        // delta sync, not a clean sync. populate our lists of local changes.
        qCDebug(lcSocialPluginTrace) << "Loading existing data given delta sync method";
        m_storage->loadNotebookIncidences(googleNotebook->uid());
        m_storage->allIncidences(&allList, googleNotebook->uid());
        m_storage->insertedIncidences(&addedList, QDateTime(since), googleNotebook->uid());
        m_storage->modifiedIncidences(&updatedList, QDateTime(since), googleNotebook->uid());

        // mkcal's implementation of deletedIncidences() is unusual.  It returns any event
        // which was deleted after the second (datetime) parameter, IF AND ONLY IF
        // it was created before that same datetime.
        // Unfortunately, mkcal also only supports second-resolution datetimes, which means
        // that the "last sync timestamp" cannot effectively be used as the parameter, since
        // any events which were added to the database due to the previous sync cycle
        // will (most likely) have been added within 1 second of the sync anchor timestamp.
        // To work around this, we need to retrieve deleted incidences twice, and unite them.
        m_storage->deletedIncidences(&deletedList, QDateTime(since), googleNotebook->uid());
        m_storage->deletedIncidences(&extraDeletedList, QDateTime(since).addSecs(1), googleNotebook->uid());
        uniteIncidenceLists(extraDeletedList, &deletedList);

        Q_FOREACH(const KCalendarCore::Incidence::Ptr incidence, allList) {
            if (incidence.isNull()) {
                qCDebug(lcSocialPlugin) << "Ignoring null incidence returned from allIncidences()";
                continue;
            }
            KCalendarCore::Event::Ptr eventPtr = m_calendar->event(incidence->uid(), incidence->recurrenceId());
            QString gcalId = gCalEventId(incidence);
            if (gcalId.isEmpty() && upsyncedUidMapping.contains(incidence->uid())) {
                // partially upsynced artifact.  It may need to be updated with gcalId comment field.
                gcalId = upsyncedUidMapping.value(incidence->uid());
                partialUpsyncArtifactsNeedingUpdate.insert(gcalId);
            }
            if (gcalId.size() && eventPtr) {
                qCDebug(lcSocialPluginTrace) << "Have local event:" << gcalId << "," << eventPtr->uid() << ":" << eventPtr->recurrenceId().toString();
                allMap.insert(gcalId, eventPtr);
            } // else, newly added locally, no gcalId yet.
        }

        Q_FOREACH(const KCalendarCore::Incidence::Ptr incidence, updatedList) {
            if (incidence.isNull()) {
                qCDebug(lcSocialPlugin) << "Ignoring null incidence returned from modifiedIncidences()";
                continue;
            }
            KCalendarCore::Event::Ptr eventPtr = m_calendar->event(incidence->uid(), incidence->recurrenceId());
            QString gcalId = gCalEventId(incidence);
            if (gcalId.isEmpty() && upsyncedUidMapping.contains(incidence->uid())) {
                // TODO: can this codepath be hit?  If it was a partial upsync artifact,
                //       shouldn't it be reported as a local+remote addition, not local modification?
                // partially upsynced artifact
                gcalId = upsyncedUidMapping.value(incidence->uid());
                partialUpsyncArtifactsNeedingUpdate.remove(gcalId); // will already update due to local change.
            }
            if (gcalId.size() && eventPtr) {
                qCDebug(lcSocialPlugin) << "Have local modification:" << incidence->uid() << "in" << calendarId;
                updatedMap.insert(gcalId, eventPtr);
            } // else, newly added+updated locally, no gcalId yet.
        }
        Q_FOREACH(const KCalendarCore::Incidence::Ptr incidence, deletedList) {
            if (incidence.isNull()) {
                qCDebug(lcSocialPlugin) << "Ignoring null incidence returned from deletedIncidences()";
                continue;
            }
            QString gcalId = gCalEventId(incidence);
            if (gcalId.isEmpty() && upsyncedUidMapping.contains(incidence->uid())) {
                // TODO: can this codepath be hit?  If it was a partial upsync artifact,
                //       shouldn't it be reported as a local+remote addition, not local deletion?
                // partially upsynced artifact
                gcalId = upsyncedUidMapping.value(incidence->uid());
                partialUpsyncArtifactsNeedingUpdate.remove(gcalId); // doesn't need update due to deletion.
            }
            if (gcalId.size()) {
                // Now we check to see whether this event was deleted due to a clean-sync (notebook removal).
                // If so, then another event (with the same gcalId association) should have been ADDED at the
                // same time, to fulfil clean-sync semantics (because the notebook uid is maintained).
                // If so, we treat it as a modification rather than delete+add pair.
                if (allMap.contains(gcalId)) {
                    // note: this works because gcalId is different for base series vs persistent occurrence of series.
                    qCDebug(lcSocialPlugin) << "Have local deletion+addition from cleansync:" << gcalId << "in" << calendarId;
                    cleanSyncDeletionAdditions.insert(gcalId);
                } else {
                    // otherwise, it's a real local deletion.
                    qCDebug(lcSocialPlugin) << "Have local deletion:" << incidence->uid() << "in" << calendarId;
                    deletedMap.insert(gcalId, qMakePair(incidence->uid(), incidence->recurrenceId()));
                    updatedMap.remove(gcalId); // don't upsync updates to deleted events.
                    m_deletedGcalIdToIncidence.insert(gcalId, incidence);
                }
            } // else, newly added+deleted locally, no gcalId yet.
        }
    } else {
        if (googleNotebook.isNull()) {
            qCDebug(lcSocialPluginTrace) << "No local notebook exists for remote; no existing data to load.";
        }
    }

    // apply the conflict resolution strategy to remove any local or remote changes which should be dropped.
    int discardedLocalAdditions = 0, discardedLocalModifications = 0, discardedLocalRemovals = 0;
    int remoteAdditions = 0, remoteModifications = 0, remoteRemovals = 0, discardedRemoteModifications = 0, discardedRemoteRemovals = 0;
    QHash<QString, QJsonObject> unchangedRemoteModifications; // gcalId to eventData.
    QStringList remoteAdditionIds;

    // For each each of the events downloaded from the server, determine
    // if the remote change invalidates a local change, or if a local
    // deletion invalidates the remote change.
    // Otherwise, cache the remote change for later storage to local db.
    foreach (const QJsonObject &eventData, eventObjects) {
        QString eventId = eventData.value(QLatin1String("id")).toVariant().toString();
        QString parentId = eventData.value(QLatin1String("recurringEventId")).toVariant().toString();
        bool eventWasDeletedRemotely = eventData.value(QLatin1String("status")).toVariant().toString() == QString::fromLatin1("cancelled");
        if (eventWasDeletedRemotely) {
            // if modified locally and deleted on server side, don't upsync modifications
            if (allMap.contains(eventId)) {
                // currently existing base event or persistent occurrence which was deleted server-side
                remoteRemovals++;
                qCDebug(lcSocialPlugin) << "Have remote series deletion:" << eventId << "in" << calendarId;
                m_changesFromDownsync.insertMulti(calendarId, qMakePair<GoogleCalendarSyncAdaptor::ChangeType, QJsonObject>(GoogleCalendarSyncAdaptor::Delete, eventData));
                if (updatedMap.contains(eventId)) {
                    qCDebug(lcSocialPlugin) << "Discarding local event modification:" << eventId << "due to remote deletion";
                    updatedMap.remove(eventId); // discard any local modifications to this event, don't upsync.
                    discardedLocalModifications++;
                }
                // also discard the event from the locally added list if it is reported there.
                // this can happen due to cleansync, or the overlap in the sync date due to mkcal resolution issue.
                for (int i = 0; i < addedList.size(); ++i) {
                    KCalendarCore::Incidence::Ptr addedEvent = addedList[i];
                    if (addedEvent.isNull()) {
                        qCDebug(lcSocialPlugin) << "Disregarding local event addition due to null state";
                        continue;
                    }
                    const QString &gcalId(gCalEventId(addedEvent));
                    if (gcalId == eventId) {
                        qCDebug(lcSocialPlugin) << "Discarding local event addition:" << addedEvent->uid() << "due to remote deletion";
                        addedList.remove(i);
                        discardedLocalAdditions++;
                        break;
                    }
                }
            } else if (!parentId.isEmpty() && (allMap.contains(parentId) || remoteAdditionIds.contains(parentId))) {
                // this is a non-persistent occurrence deletion, we need to add an EXDATE to the base event.
                // we treat this as a remote modification of the base event (ie, the EXDATE addition)
                // and thus will discard any local modifications to the base event, and not upsync them.
                // TODO: use a more optimal conflict resolution strategy for this case!
                remoteRemovals++;
                qCDebug(lcSocialPlugin) << "Have remote occurrence deletion:" << eventId << "in" << calendarId;
                m_changesFromDownsync.insertMulti(calendarId, qMakePair<GoogleCalendarSyncAdaptor::ChangeType, QJsonObject>(GoogleCalendarSyncAdaptor::DeleteOccurrence, eventData));
                if (updatedMap.contains(parentId)) {
                    qCDebug(lcSocialPlugin) << "Discarding local modification to recurrence series:" << parentId << "due to remote EXDATE addition. Sub-optimal resolution strategy!";
                    updatedMap.remove(parentId);
                    discardedLocalModifications++;
                }
                // also discard the event from the locally added list if it is reported there.
                // this can happen due to cleansync, or the overlap in the sync date due to mkcal resolution issue.
                for (int i = 0; i < addedList.size(); ++i) {
                    KCalendarCore::Incidence::Ptr addedEvent = addedList[i];
                    if (addedEvent.isNull()) {
                        qCDebug(lcSocialPlugin) << "Disregarding local event addition due to null state";
                        continue;
                    }
                    const QString &gcalId(gCalEventId(addedEvent));
                    if (gcalId == eventId) {
                        qCDebug(lcSocialPlugin) << "Discarding local event addition:" << addedEvent->uid() << "due to remote EXDATE addition.  Sub-optimal resolution strategy!";
                        addedList.remove(i);
                        discardedLocalAdditions++;
                        break;
                    }
                }
            } else {
                // !allMap.contains(parentId)
                if (deletedMap.contains(eventId)) {
                    // remote deleted event was also deleted locally, can ignore.
                    qCDebug(lcSocialPlugin) << "Event deleted remotely:" << eventId << "was already deleted locally; discarding both local and remote deletion";
                    deletedMap.remove(eventId); // discard local deletion.
                    discardedLocalRemovals++;
                    discardedRemoteRemovals++;
                } else {
                    // remote deleted event never existed locally.
                    // this can happen due to the increased timeMin window
                    // extending to prior to the account existing on the device.
                    qCDebug(lcSocialPlugin) << "Event deleted remotely:" << eventId << "was never downsynced to device; discarding";
                    discardedRemoteRemovals++;
                }
            }
        } else if (deletedMap.contains(eventId)) {
            // remote change will be discarded due to local deletion.
            qCDebug(lcSocialPlugin) << "Discarding remote event modification:" << eventId << "due to local deletion";
            discardedRemoteModifications++;
        } else if (allMap.contains(eventId)) {
            // remote modification of an existing event.
            KCalendarCore::Event::Ptr event = allMap.value(eventId);
            bool changed = false;
            if (partialUpsyncArtifactsNeedingUpdate.contains(eventId)) {
                // This event was partially upsynced and then connectivity died before we committed
                // and updated its comment field with the gcalId it was given by the remote server.
                // During this sync cycle, we will update it by assuming remote modification.
                // Note: this will lose any local changes made since it was partially-upsynced,
                // however the alternative is to lose remote changes made since then...
                // So we stick with our "prefer-remote" conflict resolution strategy here.
                qCDebug(lcSocialPlugin) << "Reloading partial upsync artifact:" << eventId << "from server as a modification";
                changed = true;
            } else {
                qCDebug(lcSocialPlugin) << "Determining if remote data differs from local data for event" << eventId << "in" << calendarId;
                if (event.isNull()) {
                    qCDebug(lcSocialPlugin) << "Unable to find local event:" << eventId << ", marking as changed.";
                    changed = true;
                } else {
                    changed = remoteModificationIsReal(eventData, event);
                }
            }
            if (!changed) {
                // Not a real change.  We discard this remote modification,
                // but we track it so that we can detect spurious local modifications.
                qCDebug(lcSocialPlugin) << "Discarding remote event modification:" << eventId << "in" << calendarId << "as spurious";
                unchangedRemoteModifications.insert(eventId, eventData);
                discardedRemoteModifications++;
            } else {
                qCDebug(lcSocialPlugin) << "Have remote modification:" << eventId << "in" << calendarId;
                remoteModifications++;
                m_changesFromDownsync.insertMulti(calendarId, qMakePair<GoogleCalendarSyncAdaptor::ChangeType, QJsonObject>(GoogleCalendarSyncAdaptor::Modify, eventData));
                if (updatedMap.contains(eventId)) {
                    // if both local and server were modified, prefer server.
                    qCDebug(lcSocialPlugin) << "Discarding local event modification:" << eventId << "due to remote modification";
                    updatedMap.remove(eventId);
                    discardedLocalModifications++;
                }
                // also discard the event from the locally added list if it is reported there.
                // this can happen due to cleansync, or the overlap in the sync date due to mkcal resolution issue.
                for (int i = 0; i < addedList.size(); ++i) {
                    KCalendarCore::Incidence::Ptr addedEvent = addedList[i];
                    if (addedEvent.isNull()) {
                        qCDebug(lcSocialPlugin) << "Disregarding local event addition due to null state";
                        continue;
                    }
                    const QString &gcalId(gCalEventId(addedEvent));
                    if (gcalId == eventId) {
                        qCDebug(lcSocialPlugin) << "Discarding local event addition:" << addedEvent->uid() << "due to remote modification";
                        addedList.remove(i);
                        discardedLocalAdditions++;
                        break;
                    }
                }
            }
        } else {
            // pure remote addition. remote additions cannot invalidate local changes.
            // note that we have already detected (and dealt with) partial-upsync-artifacts
            // which would have been reported from the remote server as additions.
            qCDebug(lcSocialPlugin) << "Have remote addition:" << eventId << "in" << calendarId;
            remoteAdditions++;
            m_changesFromDownsync.insertMulti(calendarId, qMakePair<GoogleCalendarSyncAdaptor::ChangeType, QJsonObject>(GoogleCalendarSyncAdaptor::Insert, eventData));
            remoteAdditionIds.append(eventId);
        }
    }

    qCInfo(lcSocialPlugin) << "Delta downsync from Google calendar" << calendarId << "for account" << m_accountId << ":" <<
                     "remote A/M/R: " << remoteAdditions << "/" << remoteModifications << "/" << remoteRemovals <<
                     "after discarding M/R:" << discardedRemoteModifications << "/" << discardedRemoteRemovals <<
                     "due to local deletions or identical data";

    if (upsyncEnabled) {
        // Now build the local-changes-to-upsync data structures.
        int localAdded = 0, localModified = 0, localRemoved = 0;

        // first, queue up deletions.
        Q_FOREACH (const QString &deletedGcalId, deletedMap.keys()) {
            QString incidenceUid = deletedMap.value(deletedGcalId).first;
            QDateTime recurrenceId = deletedMap.value(deletedGcalId).second;
            localRemoved++;
            qCDebug(lcSocialPluginTrace) << "queueing upsync deletion for gcal id:" << deletedGcalId;
            UpsyncChange deletion;
            deletion.accessToken = accessToken;
            deletion.upsyncType = GoogleCalendarSyncAdaptor::Delete;
            deletion.kcalEventId = incidenceUid;
            deletion.recurrenceId = recurrenceId;
            deletion.calendarId = calendarId;
            deletion.eventId = deletedGcalId;
            changesToUpsync.append(deletion);
        }

        // second, queue up modifications.
        Q_FOREACH (const QString &updatedGcalId, updatedMap.keys()) {
            KCalendarCore::Event::Ptr event = updatedMap.value(updatedGcalId);
            if (event) {
                QJsonObject localEventData = kCalToJson(event, m_icalFormat);
                if (unchangedRemoteModifications.contains(updatedGcalId)
                        && !localModificationIsReal(localEventData, unchangedRemoteModifications.value(updatedGcalId), m_serverCalendarIdToDefaultReminderTimes.value(calendarId), m_icalFormat)) {
                    // this local modification is spurious.  It may have been reported
                    // due to the timestamp resolution issue, but in any case the
                    // event does not differ from the remote one.
                    qCDebug(lcSocialPlugin) << "Discarding local event modification:" << event->uid() << event->recurrenceId().toString()
                                      << "as spurious, for gcalId:" << updatedGcalId;
                    discardedLocalModifications++;
                    continue;
                }
                localModified++;
                QByteArray eventBlob = QJsonDocument(localEventData).toJson();
                qCDebug(lcSocialPluginTrace) << "queueing upsync modification for gcal id:" << updatedGcalId;
                traceDumpStr(QString::fromUtf8(eventBlob));
                UpsyncChange modification;
                modification.accessToken = accessToken;
                modification.upsyncType = GoogleCalendarSyncAdaptor::Modify;
                modification.kcalEventId = event->uid();
                modification.recurrenceId = event->recurrenceId();
                modification.calendarId = calendarId;
                modification.eventId = updatedGcalId;
                modification.eventData = eventBlob;
                changesToUpsync.append(modification);
            }
        }

        // move parent insertions before recurrence exclusion insertions
        reorderAdditions(addedList);

        // finally, queue up insertions.
        Q_FOREACH (KCalendarCore::Incidence::Ptr incidence, addedList) {
            KCalendarCore::Event::Ptr event = m_calendar->event(incidence->uid(), incidence->recurrenceId());
            if (event) {
                if (upsyncedUidMapping.contains(incidence->uid())) {
                    const QString &eventId(upsyncedUidMapping.value(incidence->uid()));
                    if (partialUpsyncArtifactsNeedingUpdate.contains(eventId)) {
                        // We have already handled this one, by treating it as a remote modification, above.
                        qCDebug(lcSocialPlugin) << "Discarding partial upsync artifact local addition:" << eventId;
                        discardedLocalAdditions++;
                        continue;
                    }
                    // should never be hit.  bug in plugin code.
                    qCWarning(lcSocialPlugin) << "Not discarding partial upsync artifact local addition due to data inconsistency:" << eventId;
                }
                const QString gcalId = gCalEventId(event);
                if (!gcalId.isEmpty() && !event->hasRecurrenceId()) {
                    if (cleanSyncDeletionAdditions.contains(gcalId)) {
                        // this event was deleted+re-added due to clean sync.  treat it as a local modification
                        // of the remote event.  Note: we cannot update the extended UID property in the remote
                        // event, because multiple other devices may depend on it.  When we downsynced the event
                        // for the re-add, we should have re-used the old uid.
                        qCDebug(lcSocialPlugin) << "Converting local addition to modification due to clean-sync semantics";
                    } else {
                        // this event was previously downsynced from the remote in the last sync cycle.
                        // check to see whether it has changed locally since we downsynced it.
                        if (event->lastModified() < since) {
                            qCDebug(lcSocialPlugin) << "Discarding local event addition:" << event->uid() << event->recurrenceId().toString() << "as spurious due to downsync, for gcalId:" << gcalId;
                            qCDebug(lcSocialPlugin) << "Last modified:" << event->lastModified() << "<" << since;
                            discardedLocalModifications++;
                            continue;
                        }
                        // we treat it as a local modification (as it has changed locally since it was downsynced).
                        qCDebug(lcSocialPlugin) << "Converting local addition to modification due to it being a previously downsynced event";
                    }
                    // convert the local event to a JSON object.
                    QJsonObject localEventData = kCalToJson(event, m_icalFormat);
                    // check to see if this differs from some discarded remote modification.
                    // if it does not, then the remote and local are identical, and it's only
                    // being reported as a local addition/modification due to the "since" timestamp
                    // overlap.
                    if (unchangedRemoteModifications.contains(gcalId)
                            && !localModificationIsReal(localEventData, unchangedRemoteModifications.value(gcalId),
                                                        m_serverCalendarIdToDefaultReminderTimes.value(calendarId),
                                                        m_icalFormat)) {
                        // this local addition is spurious.  It may have been reported
                        // due to the timestamp resolution issue, but in any case the
                        // event does not differ from the remote one which is already updated.
                        qCDebug(lcSocialPlugin) << "Discarding local event modification:" << event->uid() << event->recurrenceId().toString()
                                          << "as spurious, for gcalId:" << gcalId;
                        discardedLocalModifications++;
                        continue;
                    }
                    localModified++;
                    QByteArray eventBlob = QJsonDocument(localEventData).toJson();
                    qCDebug(lcSocialPluginTrace) << "queueing upsync modification for gcal id:" << gcalId;
                    traceDumpStr(QString::fromUtf8(eventBlob));
                    UpsyncChange modification;
                    modification.accessToken = accessToken;
                    modification.upsyncType = GoogleCalendarSyncAdaptor::Modify;
                    modification.kcalEventId = event->uid();
                    modification.recurrenceId = event->recurrenceId();
                    modification.calendarId = calendarId;
                    modification.eventId = gcalId;
                    modification.eventData = eventBlob;
                    changesToUpsync.append(modification);
                } else {
                    localAdded++;
                    // This is an insertion, so we may need it to run after a parent insertion completed
                    queueSequencedInsertion(changesToUpsync, event, calendarId, accessToken);
                }
            }
        }

        qCInfo(lcSocialPlugin) << "Delta upsync with Google calendar" << calendarId << "for account" << m_accountId << ":" <<
                         "local A/M/R:" << localAdded << "/" << localModified << "/" << localRemoved <<
                         "after discarding A/M/R:" << discardedLocalAdditions << "/" << discardedLocalModifications << "/" << discardedLocalRemovals <<
                         "due to remote changes or identical data";
    }

    return changesToUpsync;
}

void GoogleCalendarSyncAdaptor::queueSequencedInsertion(QList<UpsyncChange> &changesToUpsync,
                                                        const KCalendarCore::Event::Ptr event,
                                                        const QString &calendarId,
                                                        const QString &accessToken)
{
    // Last parameter true -> "insert extended UID property"
    QJsonObject eventJson = kCalToJson(event, m_icalFormat, true);

    // Check whether the insertion needs to be sequenced after its parent
    QString insertionGcalId;
    UpsyncChange *parentChange = nullptr;
    if (event->hasRecurrenceId()) {
        // This event has a parent, so we must check if it needs to be sequenced
        for (UpsyncChange &change : changesToUpsync) {
            if ((change.upsyncType == Insert) && (change.kcalEventId == event->uid()) && (change.recurrenceId.isNull())) {
                parentChange = &change;
                break;
            }
        }

        // Use our generated GCalId to refer to the parent
        if (parentChange && !parentChange->eventId.isEmpty()) {
            // Replace the recurringEventId
            eventJson.insert(QLatin1String("recurringEventId"), parentChange->eventId);
        }
    } else {
        // There will be no gcalId yet, so we can generate and add our own
        insertionGcalId = generate_uuid();
        qCDebug(lcSocialPlugin) << "Generated id for new event:" << insertionGcalId;
        eventJson.insert(QLatin1String("id"), insertionGcalId);
    }

    qCDebug(lcSocialPluginTrace) << "queueing up insertion for local id:" << event->uid();
    UpsyncChange insertion;
    insertion.accessToken = accessToken;
    insertion.upsyncType = GoogleCalendarSyncAdaptor::Insert;
    insertion.kcalEventId = event->uid();
    insertion.recurrenceId = event->recurrenceId();
    insertion.calendarId = calendarId;
    insertion.eventId = insertionGcalId;
    insertion.eventData = QJsonDocument(eventJson).toJson();
    traceDumpStr(QString::fromUtf8(insertion.eventData));

    // At this point we either add the upsync change to the default queue, or to the sequenced queue
    if (parentChange && !parentChange->eventId.isEmpty()) {
        qCDebug(lcSocialPlugin) << "Sequencing event after its parent:" << parentChange->eventId;
        // Add the insertion to the appropriate sublist
        m_sequenced.insertMulti(parentChange->eventId, insertion);
    } else {
        qCDebug(lcSocialPlugin) << "Upsyncing event in first round without sequencing:" << event->uid() << event->summary();
        changesToUpsync.append(insertion);
    }
}

void GoogleCalendarSyncAdaptor::upsyncChanges(const UpsyncChange &changeToUpsync)
{
    const QString &accessToken = changeToUpsync.accessToken;
    GoogleCalendarSyncAdaptor::ChangeType upsyncType = changeToUpsync.upsyncType;
    const QString &kcalEventId = changeToUpsync.kcalEventId;
    const QDateTime &recurrenceId = changeToUpsync.recurrenceId;
    const QString &calendarId = changeToUpsync.calendarId;
    const QString &eventId = changeToUpsync.eventId;
    const QByteArray &eventData = changeToUpsync.eventData;

    QUrl requestUrl = upsyncType == GoogleCalendarSyncAdaptor::Insert
                    ? QUrl(QString::fromLatin1("https://www.googleapis.com/calendar/v3/calendars/%1/events").arg(calendarId))
                    : QUrl(QString::fromLatin1("https://www.googleapis.com/calendar/v3/calendars/%1/events/%2").arg(calendarId).arg(eventId));

    QNetworkRequest request(requestUrl);
    request.setRawHeader("GData-Version", "3.0");
    request.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                         QString(QLatin1String("Bearer ") + accessToken).toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QVariant::fromValue<QString>(QString::fromLatin1("application/json")));

    QNetworkReply *reply = 0;

    QString upsyncTypeStr;
    switch (upsyncType) {
        case GoogleCalendarSyncAdaptor::Insert:
            upsyncTypeStr = QString::fromLatin1("Insert");
            reply = m_networkAccessManager->post(request, eventData);
            break;
        case GoogleCalendarSyncAdaptor::Modify:
            upsyncTypeStr = QString::fromLatin1("Modify");
            reply = m_networkAccessManager->put(request, eventData);
            break;
        case GoogleCalendarSyncAdaptor::Delete:
            upsyncTypeStr = QString::fromLatin1("Delete");
            reply = m_networkAccessManager->deleteResource(request);
            break;
        default:
            qCWarning(lcSocialPlugin) << "UNREACHBLE - upsyncing non-change"; // always an error.
            m_syncSucceeded = false;
            return;
    }

    // we're performing a request.  Increment the semaphore so that we know we're still busy.
    incrementSemaphore(m_accountId);

    if (reply) {
        reply->setProperty("accountId", m_accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("upsyncType", static_cast<int>(upsyncType));
        reply->setProperty("kcalEventId", kcalEventId);
        reply->setProperty("recurrenceId", recurrenceId);
        reply->setProperty("calendarId", calendarId);
        reply->setProperty("eventId", eventId);
        reply->setProperty("eventData", eventData);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)),
                this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)),
                this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(upsyncFinishedHandler()));

        setupReplyTimeout(m_accountId, reply);

        qCDebug(lcSocialPlugin) << "upsyncing change:" << upsyncTypeStr <<
                          "to calendarId:" << calendarId <<
                          "of account" << m_accountId << "to" <<
                          request.url().toString();
        traceDumpStr(QString::fromUtf8(eventData));
    } else {
        qCWarning(lcSocialPlugin) << "unable to request upsync for calendar" << calendarId <<
                          "from Google account with id" << m_accountId;
        m_syncSucceeded = false;
        decrementSemaphore(m_accountId);
    }
}

void GoogleCalendarSyncAdaptor::reInsertWithRandomId(const QNetworkReply *reply)
{
    Q_ASSERT(reply->property("accountId").toInt() == m_accountId);
    QString accessToken = reply->property("accessToken").toString();
    ChangeType upsyncType = ChangeType(reply->property("upsyncType").toInt());
    QString kcalEventId = reply->property("kcalEventId").toString();
    QDateTime recurrenceId = reply->property("recurrenceId").toDateTime();
    QString calendarId = reply->property("calendarId").toString();
    QString eventId = reply->property("eventId").toString();
    QByteArray eventData = reply->property("eventData").toByteArray();

    // The gcalId we chose randomly collided, so we should try with another
    qCDebug(lcSocialPluginTrace) << "GCalId collision, try with something different";
    QString insertionGcalId = generate_uuid();

    qCDebug(lcSocialPlugin) << "Generated id for new event:" << insertionGcalId;

    // Update the parent's sequenced events
    if (m_sequenced.contains(eventId)) {
        // Moving values in-place is problemmatic in case (eventId == insertionGuid) so copy them
        QList<UpsyncChange> changesToUpsync = m_sequenced.values(eventId);
        m_sequenced.remove(eventId);
        for (UpsyncChange &changeToUpsync : changesToUpsync) {
            qCDebug(lcSocialPlugin) << "Updating sequenced gcalId for event" << changeToUpsync.kcalEventId << "recurrenceId" << changeToUpsync.recurrenceId;
            changeToUpsync.eventData = jsonReplaceValue(changeToUpsync.eventData, "recurringEventId", insertionGcalId);
            m_sequenced.insertMulti(insertionGcalId, changeToUpsync);
        }
    }

    UpsyncChange changeToUpsync;
    changeToUpsync.accessToken = accessToken;
    changeToUpsync.upsyncType = upsyncType;
    changeToUpsync.kcalEventId = kcalEventId;
    changeToUpsync.recurrenceId = recurrenceId;
    changeToUpsync.calendarId = calendarId;
    changeToUpsync.eventId = insertionGcalId;
    changeToUpsync.eventData = jsonReplaceValue(eventData, "id", insertionGcalId);
    upsyncChanges(changeToUpsync);
}

void GoogleCalendarSyncAdaptor::handleErrorReply(QNetworkReply *reply)
{
    Q_ASSERT(reply->property("accountId").toInt() == m_accountId);
    ChangeType upsyncType = ChangeType(reply->property("upsyncType").toInt());
    QDateTime recurrenceId = reply->property("recurrenceId").toDateTime();
    const QByteArray &replyData = reply->readAll();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString kcalEventId = reply->property("kcalEventId").toString();

    // error occurred during request.
    qCWarning(lcSocialPlugin) << "error: calendarId:" << reply->property("calendarId").toString();
    qCWarning(lcSocialPlugin) << "error: eventId:" << reply->property("eventId").toString();
    qCWarning(lcSocialPlugin) << "error: summary:" << reply->property("summary").toString();
    qCWarning(lcSocialPlugin) << "error" << httpCode << "occurred upsyncing Google account" << m_accountId << "; got:";
    errorDumpStr(QString::fromUtf8(replyData));

    if (reply->error() == QNetworkReply::ContentOperationNotPermittedError) {
        const QString reason = getErrorReason(replyData);
        if (reason == ERROR_REASON_NON_ORGANIZER) {
            // This is an attempt to modify a shared event, and Google prevents
            // any user other than the original creator of the event from modifying those.
            // Such errors should not prevent the rest of the sync cycle from succeeding.
            qCDebug(lcSocialPluginTrace) << "Ignoring 403 due to shared calendar resource";
        } else {
            qCWarning(lcSocialPlugin) << "Usage limit reached. Please try syncing again later.";
            m_syncSucceeded = false;
        }
        flagUploadFailure(kcalEventId);
    } else if (httpCode == 410) {
        // HTTP 410 GONE "deleted"
        // The event was already deleted on the server, so continue as normal
        qCDebug(lcSocialPluginTrace) << "Event already deleted on the server, so we're now in sync";
    } else if ((httpCode == 404) && (upsyncType == ChangeType::Delete)) {
        // HTTP 404 NOT FOUND on deletion
        // The event doesn't now exist on client or server, so continue as normal
        qCDebug(lcSocialPluginTrace) << "Event deleted doesn't exist on the server, so we're now in sync";
    } else if (httpCode == 409) {
        // HTTP 409 CONFLICT "The requested identifier already exists"
        // This should be a super-rare occurrence
        // We only generate gcalIds for insertions without recurrenceIds
        Q_ASSERT(upsyncType == GoogleCalendarSyncAdaptor::Insert);
        Q_ASSERT(recurrenceId.isNull());
        ++m_collisionErrorCount;

        if (m_collisionErrorCount < COLLISION_ERROR_MAX_CONSECUTIVE) {
            reInsertWithRandomId(reply);
        } else {
            qCDebug(lcSocialPluginTrace) << "Reached" << m_collisionErrorCount << "id collisions; giving up";
            flagUploadFailure(kcalEventId);
            m_syncSucceeded = false;
        }
    } else {
        flagUploadFailure(kcalEventId);
        m_syncSucceeded = false;
    }
}

void GoogleCalendarSyncAdaptor::handleDeleteReply(QNetworkReply *reply)
{
    Q_ASSERT(reply->property("accountId").toInt() == m_accountId);
    QString kcalEventId = reply->property("kcalEventId").toString();
    QString eventId = reply->property("eventId").toString();
    const QByteArray &replyData = reply->readAll();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // we expect an empty response body on success for Delete operations
    // the only exception is if there's an error, in which case this should have been
    // picked up by the "isError" clause.
    if (replyData.isEmpty()) {
        KCalendarCore::Incidence::Ptr incidence = m_deletedGcalIdToIncidence.value(eventId);
        qCDebug(lcSocialPluginTrace) << "Deletion confirmed, purging event: " << kcalEventId;
        m_purgeList += incidence;
    } else {
        // This path should never be taken
        qCWarning(lcSocialPlugin) << "error" << httpCode << "occurred while upsyncing calendar event deletion to Google account" << m_accountId << "; got:";
        errorDumpStr(QString::fromUtf8(replyData));
        m_syncSucceeded = false;
    }
}

void GoogleCalendarSyncAdaptor::handleInsertModifyReply(QNetworkReply *reply)
{
    ChangeType upsyncType = ChangeType(reply->property("upsyncType").toInt());
    QString kcalEventId = reply->property("kcalEventId").toString();
    QDateTime recurrenceId = reply->property("recurrenceId").toDateTime();
    QString calendarId = reply->property("calendarId").toString();
    const QByteArray &replyData = reply->readAll();

    // we expect an event resource body on success for Insert/Modify requests.
    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);
    if (!ok) {
        QString typeStr = upsyncType == GoogleCalendarSyncAdaptor::Insert
                        ? QString::fromLatin1("insertion")
                        : QString::fromLatin1("modification");
        qCWarning(lcSocialPlugin) << "error occurred while upsyncing calendar event" << typeStr <<
                          "to Google account" << m_accountId << "; got:";
        errorDumpStr(QString::fromUtf8(replyData));
        flagUploadFailure(kcalEventId);
        m_syncSucceeded = false;
    } else {
        // No collision for this upsync, so reset the collision error count
        m_collisionErrorCount = 0;
        // TODO: reduce code duplication between here and the other function.
        // Search for the device Notebook matching this CalendarId
        mKCal::Notebook::Ptr googleNotebook = notebookForCalendarId(calendarId);
        if (googleNotebook.isNull()) {
            qCWarning(lcSocialPlugin) << "calendar" << calendarId << "doesn't have a notebook for Google account with id" << m_accountId;
            m_syncSucceeded = false;
        } else {
            // cache the update to this event in the local calendar
            m_storage->loadNotebookIncidences(googleNotebook->uid());
            KCalendarCore::Event::Ptr event = m_calendar->event(kcalEventId, recurrenceId);
            if (!event) {
                qCWarning(lcSocialPlugin) << "event" << kcalEventId << recurrenceId.toString() << "was deleted locally during sync of Google account with id" << m_accountId;
                m_syncSucceeded = false;
            } else {
                qCDebug(lcSocialPluginTrace) << "Local upsync response json:";
                traceDumpStr(QString::fromUtf8(replyData));
                m_changesFromUpsync.insertMulti(calendarId, qMakePair<KCalendarCore::Event::Ptr,QJsonObject>(event, parsed));
                flagUploadSuccess(kcalEventId);
            }
        }
    }
}

void GoogleCalendarSyncAdaptor::performSequencedUpsyncs(const QNetworkReply *reply)
{
    QString eventId = reply->property("eventId").toString();

    qCDebug(lcSocialPlugin) << "Performing sequenced upsyncs";

    // Trigger any sequenced upsyncs before we decrement the semaphore
    QMultiHash<QString, UpsyncChange>::const_iterator iter = m_sequenced.find(eventId);
    while (iter != m_sequenced.end() && iter.key() == eventId) {
        const UpsyncChange &changeToUpsync = iter.value();
        qCDebug(lcSocialPlugin) << "Sequenced upsync for event" << changeToUpsync.kcalEventId << "recurrenceId" << changeToUpsync.recurrenceId;
        upsyncChanges(changeToUpsync);
        ++iter;
    }
}

void GoogleCalendarSyncAdaptor::upsyncFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());

    Q_ASSERT(reply->property("accountId").toInt() == m_accountId);
    ChangeType upsyncType = ChangeType(reply->property("upsyncType").toInt());
    bool isError = reply->property("isError").toBool();

    // QNetworkReply can report an error even if there isn't one...
    if (isError && reply->error() == QNetworkReply::UnknownContentError
            && upsyncType == GoogleCalendarSyncAdaptor::Delete) {
        isError = false; // not a real error; Google returns an empty response.
    }

    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(m_accountId, reply);

    // parse the calendars' metadata from the response.
    if (isError) {
        handleErrorReply(reply);
    } else if (upsyncType == GoogleCalendarSyncAdaptor::Delete) {
        handleDeleteReply(reply);
    } else {
        // upsyncType == GoogleCalendarSyncAdaptor::Insert
        // upsyncType == GoogleCalendarSyncAdaptor::Modify
        handleInsertModifyReply(reply);
    }

    if (!isError) {
        performSequencedUpsyncs(reply);
    }

    // we're finished with this request.
    decrementSemaphore(m_accountId);
}

void GoogleCalendarSyncAdaptor::setCalendarProperties(
        mKCal::Notebook::Ptr notebook,
        const CalendarInfo &calendarInfo,
        const QString &serverCalendarId,
        int accountId,
        const QString &syncProfile,
        const QString &ownerEmail)
{
    notebook->setIsReadOnly(false);
    notebook->setName(calendarInfo.summary);
    notebook->setDescription(calendarInfo.description);
    notebook->setPluginName(QStringLiteral("google"));
    notebook->setSyncProfile(syncProfile);
    notebook->setCustomProperty(NOTEBOOK_SERVER_ID_PROPERTY, serverCalendarId);
    notebook->setCustomProperty(NOTEBOOK_EMAIL_PROPERTY, ownerEmail);
    // extra calendars have their own email addresses. using this property to pass it forward.
    notebook->setSharedWith(QStringList() << serverCalendarId);
    notebook->setAccount(QString::number(accountId));

    if (!calendarInfo.color.isEmpty()
            && notebook->customProperty(SERVER_COLOR_PROPERTY) != calendarInfo.color) {
        if (!notebook->customProperty(SERVER_COLOR_PROPERTY).isEmpty()) {
            // Override user-selected notebook color only on each server change
            // and not if there was no server color saved.
            notebook->setColor(calendarInfo.color);
        }
        notebook->setCustomProperty(SERVER_COLOR_PROPERTY, calendarInfo.color);
    }

}

void GoogleCalendarSyncAdaptor::applyRemoteChangesLocally()
{
    qCDebug(lcSocialPlugin) << "applying all remote changes to local database";
    QString emailAddress;
    QString syncProfile;
    Accounts::Account *account = Accounts::Account::fromId(m_accountManager, m_accountId, Q_NULLPTR);
    if (!account) {
        qCWarning(lcSocialPlugin) << "unable to load Google account" << m_accountId << "to retrieve settings";
    } else {
        account->selectService(m_accountManager->service(QStringLiteral("google-gmail")));
        emailAddress = account->valueAsString(QStringLiteral("emailaddress"));
        account->selectService(m_accountManager->service(QStringLiteral("google-calendars")));
        syncProfile = account->valueAsString(QStringLiteral("google.Calendars/profile_id"));
        account->deleteLater();
    }
    foreach (const QString &serverCalendarId, m_serverCalendarIdToCalendarInfo.keys()) {
        const CalendarInfo calendarInfo = m_serverCalendarIdToCalendarInfo.value(serverCalendarId);
        const QString ownerEmail = (calendarInfo.access == GoogleCalendarSyncAdaptor::Owner) ? emailAddress : QString();

        switch (calendarInfo.change) {
            case GoogleCalendarSyncAdaptor::NoChange: {
                // No changes required.  Note that this just applies to the notebook metadata;
                // there may be incidences belonging to this notebook which need modification.
                qCDebug(lcSocialPlugin) << "No metadata changes required for local notebook for server calendar:" << serverCalendarId;
                mKCal::Notebook::Ptr notebook = notebookForCalendarId(serverCalendarId);
                // We ensure anyway property values for notebooks created without.
                if (notebook && notebook->syncProfile() != syncProfile) {
                    qCDebug(lcSocialPlugin) << "Adding missing sync profile label.";
                    notebook->setSyncProfile(syncProfile);
                    m_storage->updateNotebook(notebook);
                    // Actually, we don't need to flag m_storageNeedsSave since
                    // notebook operations are immediate on storage.
                }
            } break;
            case GoogleCalendarSyncAdaptor::Insert: {
                qCDebug(lcSocialPlugin) << "Adding local notebook for new server calendar:" << serverCalendarId;
                mKCal::Notebook::Ptr notebook = mKCal::Notebook::Ptr(new mKCal::Notebook);
                setCalendarProperties(notebook, calendarInfo, serverCalendarId, m_accountId, syncProfile, ownerEmail);
                m_storage->addNotebook(notebook);
                m_storageNeedsSave = true;
            } break;
            case GoogleCalendarSyncAdaptor::Modify: {
                qCDebug(lcSocialPlugin) << "Modifications required for local notebook for server calendar:" << serverCalendarId;
                mKCal::Notebook::Ptr notebook = notebookForCalendarId(serverCalendarId);
                if (notebook.isNull()) {
                    qCWarning(lcSocialPlugin) << "unable to modify non-existent calendar:" << serverCalendarId << "for account:" << m_accountId;
                    m_syncSucceeded = false; // we don't return immediately, as we want to at least attempt to
                                             // apply other database modifications if possible, in order to leave
                                             // the local database in a usable state even after failed sync.
                } else {
                    setCalendarProperties(notebook, calendarInfo, serverCalendarId, m_accountId, syncProfile, ownerEmail);
                    m_storage->updateNotebook(notebook);
                    m_storageNeedsSave = true;
                }
            } break;
            case GoogleCalendarSyncAdaptor::Delete: {
                qCDebug(lcSocialPlugin) << "Deleting local notebook for deleted server calendar:" << serverCalendarId;
                mKCal::Notebook::Ptr notebook = notebookForCalendarId(serverCalendarId);
                if (notebook.isNull()) {
                    qCWarning(lcSocialPlugin) << "unable to delete non-existent calendar:" << serverCalendarId << "for account:" << m_accountId;
                    // m_syncSucceeded = false; // don't mark as failed, since the outcome is identical.
                } else {
                    notebook->setIsReadOnly(false);
                    m_storage->deleteNotebook(notebook);
                    m_storageNeedsSave = true;
                }
            } break;
            case GoogleCalendarSyncAdaptor::DeleteOccurrence: {
                // this codepath should never be hit.
                qCWarning(lcSocialPlugin) << "invalid DeleteOccurrence change reported for calendar:" << serverCalendarId << "from account:" << m_accountId;
            } break;
            case GoogleCalendarSyncAdaptor::CleanSync: {
                qCDebug(lcSocialPlugin) << "Deleting and recreating local notebook for clean-sync server calendar:" << serverCalendarId;
                QString notebookUid; // reuse the old notebook Uid after recreating it due to clean sync.
                // delete
                mKCal::Notebook::Ptr notebook = notebookForCalendarId(serverCalendarId);
                if (!notebook.isNull()) {
                    qCDebug(lcSocialPlugin) << "deleting notebook:" << notebook->uid() << "due to clean sync";
                    notebookUid = notebook->uid();
                    notebook->setIsReadOnly(false);
                    m_storage->deleteNotebook(notebook);
                } else {
                    qCDebug(lcSocialPlugin) << "could not find local notebook corresponding to server calendar:" << serverCalendarId;
                }
                // and then recreate.
                qCDebug(lcSocialPlugin) << "recreating notebook:" << notebook->uid() << "due to clean sync";
                notebook = mKCal::Notebook::Ptr(new mKCal::Notebook);
                if (!notebookUid.isEmpty()) {
                    notebook->setUid(notebookUid);
                }
                setCalendarProperties(notebook, calendarInfo, serverCalendarId, m_accountId, syncProfile, ownerEmail);
                m_storage->addNotebook(notebook);
                m_storageNeedsSave = true;
            } break;
        }
    }

    qCDebug(lcSocialPlugin) << "finished updating local notebooks, about to apply remote event delta locally";
    QStringList calendarsNeedingLocalChanges = m_changesFromDownsync.keys() + m_changesFromUpsync.keys();
    calendarsNeedingLocalChanges.removeDuplicates();
    Q_FOREACH (const QString &updatedCalendarId, calendarsNeedingLocalChanges) {
        // save any required changes to the local database
        updateLocalCalendarNotebookEvents(updatedCalendarId);
        m_storageNeedsSave = true;
    }
}

KCalendarCore::Event::Ptr GoogleCalendarSyncAdaptor::addDummyParent(const QJsonObject &eventData,
                                                                    const QString &parentId,
                                                                    const mKCal::Notebook::Ptr googleNotebook)
{
    if (!googleNotebook) {
        qCWarning(lcSocialPlugin) << "No google Notebook for calendar inserting:" << parentId;
        return KCalendarCore::Event::Ptr();
    }

    KCalendarCore::Event::Ptr parentEvent = KCalendarCore::Event::Ptr(new KCalendarCore::Event);
    bool changed = true;
    const QList<QDateTime> emptyExceptions;
    jsonToKCal(eventData, parentEvent, 0, m_icalFormat, emptyExceptions, &changed);
    // Clear the recurrence rule, to avoid issues when one gets added during dissociation
    parentEvent->clearRecurrence();

    clampEventTimeToSync(parentEvent);
    qCDebug(lcSocialPlugin) << "Inserting parent event with new lastModified time: " << parentEvent->lastModified().toString();
    setGCalEventId(parentEvent, parentId);

    if (!m_calendar->addEvent(parentEvent, googleNotebook->uid())) {
        qCWarning(lcSocialPlugin) << "Could not add parent occurrence to calendar:" << parentId;
        return KCalendarCore::Event::Ptr();
    }

    return parentEvent;
}

bool GoogleCalendarSyncAdaptor::applyRemoteDelete(const QString &eventId,
                                                  QMap<QString, KCalendarCore::Event::Ptr> &allLocalEventsMap)
{
    qCDebug(lcSocialPlugin) << "Event deleted remotely:" << eventId;
    KCalendarCore::Event::Ptr doomed = allLocalEventsMap.value(eventId);
    if (!m_calendar->deleteEvent(doomed)) {
        qCWarning(lcSocialPlugin) << "Unable to delete incidence: " << doomed->uid() << doomed->recurrenceId().toString();
        flagDeleteFailure(doomed->uid());
        return false;
    }
    return true;
}

bool GoogleCalendarSyncAdaptor::applyRemoteDeleteOccurence(const QString &eventId,
                                const QJsonObject &eventData,
                                QMap<QString, KCalendarCore::Event::Ptr> &allLocalEventsMap)
{
    const QString parentId = eventData.value(QLatin1String("recurringEventId")).toVariant().toString();
    const QDateTime recurrenceId = parseRecurrenceId(eventData.value("originalStartTime").toObject());

    qCDebug(lcSocialPlugin) << "Occurrence deleted remotely:" << eventId << "for recurrenceId:" << recurrenceId.toString();
    KCalendarCore::Event::Ptr event = allLocalEventsMap.value(parentId);
    if (event) {
        if (recurrenceId.isValid()) {
            event->startUpdates();
            if (event->allDay()) {
                event->recurrence()->addExDate(recurrenceId.date());
            } else {
                event->recurrence()->addExDateTime(recurrenceId);
            }
            event->endUpdates();
        } else {
            flagDeleteFailure(event->uid());
        }
    } else {
        // The parent event should never be null by this point, but we guard against it just in case
        qCWarning(lcSocialPlugin) << "Deletion failed as the parent event" << parentId << "couldn't be found";
    }
    return true;
}

bool GoogleCalendarSyncAdaptor::applyRemoteModify(const QString &eventId,
                                                  const QJsonObject &eventData,
                                                  const QString &calendarId,
                                                  QMap<QString, KCalendarCore::Event::Ptr> &allLocalEventsMap)
{
    qCDebug(lcSocialPlugin) << "Event modified remotely:" << eventId;
    KCalendarCore::Event::Ptr event = allLocalEventsMap.value(eventId);
    if (event.isNull()) {
        qCWarning(lcSocialPlugin) << "Cannot find modified event:" << eventId << "in local calendar!";
        return false;
    }
    bool changed = false; // modification, not insert, so initially changed = "false".
    const QList<QDateTime> exceptions = getExceptionInstanceDates(event);
    jsonToKCal(eventData, event, m_serverCalendarIdToDefaultReminderTimes.value(calendarId), m_icalFormat, exceptions, &changed);
    clampEventTimeToSync(event);
    qCDebug(lcSocialPlugin) << "Modified event with new lastModified time: " << event->lastModified().toString();

    return true;
}

bool GoogleCalendarSyncAdaptor::applyRemoteInsert(const QString &eventId,
                                                  const QJsonObject &eventData,
                                                  const QString &calendarId,
                                                  const QHash<QString, QString> &upsyncedUidMapping,
                                                  QMap<QString, KCalendarCore::Event::Ptr> &allLocalEventsMap)
{
    QDateTime recurrenceId = parseRecurrenceId(eventData.value("originalStartTime").toObject());
    QString parentId = eventData.value(QLatin1String("recurringEventId")).toVariant().toString();
    mKCal::Notebook::Ptr googleNotebook = notebookForCalendarId(calendarId);

    if (!googleNotebook) {
        qCWarning(lcSocialPlugin) << "No google Notebook for calendar:" << calendarId;
        return false;
    }

    KCalendarCore::Event::Ptr event;
    if (recurrenceId.isValid()) {
        // this is a persistent occurrence for an already-existing series.
        qCDebug(lcSocialPlugin) << "Persistent occurrence added remotely:" << eventId;
        KCalendarCore::Event::Ptr parentEvent = allLocalEventsMap.value(parentId);
        if (parentEvent.isNull()) {
            // it might have been newly added in this sync cycle.  Look for it from the calendar.
            QString parentEventUid = m_recurringEventIdToKCalUid.value(parentId);
            parentEvent = parentEventUid.isEmpty() ? parentEvent : m_calendar->event(parentEventUid, QDateTime());
        }

        if (parentEvent.isNull()) {
            // construct a recurring parent series for this orphan
            qCInfo(lcSocialPlugin) << "Creating parent:" << parentId << "for orphaned event:" << eventId;

            parentEvent = addDummyParent(eventData, parentId, googleNotebook);
            if (!parentEvent) {
                return false;
            }

            m_recurringEventIdToKCalUid.insert(parentId, parentEvent->uid());

            // Add to the local events map, in case there are future modifications in the same sync
            if (parentId.size()) {
                allLocalEventsMap.insert(parentId, parentEvent);
            }
        }

        // dissociate the persistent occurrence
        qCDebug(lcSocialPlugin) << "Dissociating exception from" << parentEvent->uid();
        event = dissociateSingleOccurrence(parentEvent, recurrenceId);

        if (event.isNull()) {
            qCWarning(lcSocialPlugin) << "Could not dissociate occurrence from recurring event:" << parentId << recurrenceId.toString();
            return false;
        }
    } else {
        // this is a new event in its own right.
        qCDebug(lcSocialPlugin) << "Event added remotely:" << eventId;
        event = KCalendarCore::Event::Ptr(new KCalendarCore::Event);
        // check to see if another Sailfish OS device uploaded this event.
        // if so, we want to use the same local UID it did.
        QString localUid = eventData.value(QLatin1String("extendedProperties")).toObject()
                                    .value(QLatin1String("private")).toObject()
                                    .value("x-jolla-sociald-mkcal-uid").toVariant().toString();
        if (localUid.size()) {
            // either this event was uploaded by a different Sailfish OS device,
            // in which case we should re-use the uid it used;
            // or it was uploaded by this device from a different notebook,
            // and then the event was copied to a different calendar via
            // the Google web UI - in which case we need to use a different
            // uid as mkcal doesn't support a single event being stored in
            // multiple notebooks.
            m_storage->load(localUid); // the return value is useless, returns true even if count == 0
            KCalendarCore::Event::Ptr checkLocalUidEvent = m_calendar->event(localUid, QDateTime());
            if (!checkLocalUidEvent) {
                qCDebug(lcSocialPlugin) << "Event" << eventId << "was synced by another Sailfish OS device, reusing local uid:" << localUid;
                event->setUid(localUid);
            }
        }
    }
    bool changed = true; // set to true as it's an addition, no need to check for delta.
    const QList<QDateTime> exceptions = getExceptionInstanceDates(event);
    jsonToKCal(eventData, event, m_serverCalendarIdToDefaultReminderTimes.value(calendarId), m_icalFormat, exceptions, &changed); // direct conversion
    clampEventTimeToSync(event);
    qCDebug(lcSocialPlugin) << "Inserting event with new lastModified time: " << event->lastModified().toString();

    if (!m_calendar->addEvent(event, googleNotebook->uid())) {
        qCWarning(lcSocialPlugin) << "Could not add dissociated occurrence to calendar:" << parentId << recurrenceId.toString();
        return false;
    }
    m_recurringEventIdToKCalUid.insert(eventId, event->uid());

    // Add to the local events map, in case there are future modifications in the same sync
    QString gcalId = gCalEventId(event);
    if (gcalId.isEmpty()) {
        gcalId = upsyncedUidMapping.value(event->uid());
    }
    if (gcalId.size() && event) {
        allLocalEventsMap.insert(gcalId, event);
    }

    return true;
}

void GoogleCalendarSyncAdaptor::updateLocalCalendarNotebookEvents(const QString &calendarId)
{
    QList<QPair<GoogleCalendarSyncAdaptor::ChangeType, QJsonObject> > changesFromDownsyncForCalendar = m_changesFromDownsync.values(calendarId);
    QList<QPair<KCalendarCore::Event::Ptr, QJsonObject> > changesFromUpsyncForCalendar = m_changesFromUpsync.values(calendarId);
    if (changesFromDownsyncForCalendar.isEmpty() && changesFromUpsyncForCalendar.isEmpty()) {
        qCDebug(lcSocialPlugin) << "No remote changes to apply for calendar:" << calendarId << "for Google account:" << m_accountId;
        return; // no remote changes to apply.
    }

    // Set notebook writeable locally.
    mKCal::Notebook::Ptr googleNotebook = notebookForCalendarId(calendarId);
    if (!googleNotebook) {
        qCWarning(lcSocialPlugin) << "no local notebook associated with calendar:" << calendarId << "from account:" << m_accountId << "to update!";
        m_syncSucceeded = false;
        return;
    }

    KCalendarCore::Incidence::List allLocalEventsList;
    m_storage->loadNotebookIncidences(googleNotebook->uid());
    m_storage->allIncidences(&allLocalEventsList, googleNotebook->uid());

    // write changes required to complete downsync to local database
    googleNotebook->setIsReadOnly(false);
    if (!changesFromDownsyncForCalendar.isEmpty()) {
        // build the partial-upsync-artifact mapping for this set of changes.
        QHash<QString, QString> upsyncedUidMapping;
        for (int i = 0; i < changesFromDownsyncForCalendar.size(); ++i) {
            const QPair<GoogleCalendarSyncAdaptor::ChangeType, QJsonObject> &remoteChange(changesFromDownsyncForCalendar[i]);
            QString gcalId = remoteChange.second.value(QLatin1String("id")).toVariant().toString();
            QString upsyncedUid = remoteChange.second.value(QLatin1String("extendedProperties")).toObject()
                                                     .value(QLatin1String("private")).toObject()
                                                     .value("x-jolla-sociald-mkcal-uid").toVariant().toString();
            if (!upsyncedUid.isEmpty() && !gcalId.isEmpty()) {
                upsyncedUidMapping.insert(upsyncedUid, gcalId);
            }
        }

        // build up map of gcalIds to local events for this change set
        QMap<QString, KCalendarCore::Event::Ptr> allLocalEventsMap;
        Q_FOREACH(const KCalendarCore::Incidence::Ptr incidence, allLocalEventsList) {
            if (incidence.isNull()) {
                continue;
            }
            KCalendarCore::Event::Ptr eventPtr = m_calendar->event(incidence->uid(), incidence->recurrenceId());
            QString gcalId = gCalEventId(incidence);
            if (gcalId.isEmpty()) {
                gcalId = upsyncedUidMapping.value(incidence->uid());
            }
            if (gcalId.size() && eventPtr) {
                allLocalEventsMap.insert(gcalId, eventPtr);
            }
        }

        // re-order remote changes so that additions of recurring series happen before additions of exception occurrences.
        // otherwise, the parent event may not exist when we attempt to insert the exception.
        // similarly, re-order remote deletions of exceptions so that they occur before remote deletions of series.
        // So we will have this ordering:
        // 1. Remote parent additions
        // 2. Remote exception deletions
        // 3. Remote exception additions
        // 4. Remote parent deletions
        QList<QPair<GoogleCalendarSyncAdaptor::ChangeType, QJsonObject> > reorderedChangesFromDownsyncForCalendar;
        for (int i = 0; i < changesFromDownsyncForCalendar.size(); ++i) {
            const QPair<GoogleCalendarSyncAdaptor::ChangeType, QJsonObject> &remoteChange(changesFromDownsyncForCalendar[i]);
            QString parentId = remoteChange.second.value(QLatin1String("recurringEventId")).toVariant().toString();
            if (!parentId.isEmpty()) {
                if (remoteChange.first == GoogleCalendarSyncAdaptor::Delete || remoteChange.first == GoogleCalendarSyncAdaptor::DeleteOccurrence) {
                    reorderedChangesFromDownsyncForCalendar.prepend(remoteChange);
                } else {
                    reorderedChangesFromDownsyncForCalendar.append(remoteChange);
                }
            }
        }
        for (int i = 0; i < changesFromDownsyncForCalendar.size(); ++i) {
            const QPair<GoogleCalendarSyncAdaptor::ChangeType, QJsonObject> &remoteChange(changesFromDownsyncForCalendar[i]);
            QString parentId = remoteChange.second.value(QLatin1String("recurringEventId")).toVariant().toString();
            if (parentId.isEmpty()) {
                if (remoteChange.first == GoogleCalendarSyncAdaptor::Delete) {
                    reorderedChangesFromDownsyncForCalendar.append(remoteChange);
                } else {
                    reorderedChangesFromDownsyncForCalendar.prepend(remoteChange);
                }
            }
        }

        // apply the remote changes locally.
        for (int i = 0; i < reorderedChangesFromDownsyncForCalendar.size(); ++i) {
            const QPair<GoogleCalendarSyncAdaptor::ChangeType, QJsonObject> &remoteChange(reorderedChangesFromDownsyncForCalendar[i]);
            const QJsonObject eventData(remoteChange.second);
            const QString eventId = eventData.value(QLatin1String("id")).toVariant().toString();
            bool success = true;
            switch (remoteChange.first) {
                case GoogleCalendarSyncAdaptor::Delete: {
                    // currently existing base event or persistent occurrence which needs deletion
                    success = applyRemoteDelete(eventId, allLocalEventsMap);
                } break;
                case GoogleCalendarSyncAdaptor::DeleteOccurrence: {
                    // this is a non-persistent occurrence, we need to add an EXDATE to the base event.
                    success = applyRemoteDeleteOccurence(eventId, eventData, allLocalEventsMap);
                } break;
                case GoogleCalendarSyncAdaptor::Modify: {
                    // An existing event was modified remotely
                    success = applyRemoteModify(eventId, eventData, calendarId, allLocalEventsMap);
                } break;
                case GoogleCalendarSyncAdaptor::Insert: {
                    // add a new local event for the remote addition.
                    success = applyRemoteInsert(eventId, eventData, calendarId, upsyncedUidMapping,
                                                allLocalEventsMap);
                } break;
                default: break;
            }

            if (success) {
                flagUpdateSuccess(eventId);
            } else {
                m_syncSucceeded = false;
            }
        }
    }

    // write changes required to complete upsync to the local database
    for (int i = 0; i < changesFromUpsyncForCalendar.size(); ++i) {
        const QPair<KCalendarCore::Event::Ptr, QJsonObject> &remoteChange(changesFromUpsyncForCalendar[i]);
        KCalendarCore::Event::Ptr event(remoteChange.first);
        const QJsonObject eventData(remoteChange.second);
        // all changes are modifications to existing events, since it was an upsync response.
        bool changed = false;
        qCDebug(lcSocialPlugin) << "Updating event:" << event->summary();
        const QList<QDateTime> exceptions = getExceptionInstanceDates(event);
        jsonToKCal(eventData, event, m_serverCalendarIdToDefaultReminderTimes.value(calendarId), m_icalFormat, exceptions, &changed);
        if (changed) {
            flagUpdateSuccess(event->uid());
            qCDebug(lcSocialPlugin) << "Two-way calendar sync with account" << m_accountId << ": re-updating event:" << event->summary();
        }
    }
}

void GoogleCalendarSyncAdaptor::clampEventTimeToSync(KCalendarCore::Event::Ptr event) const
{
    if (event) {
        // Don't allow the event created time to fall after the sync time
        if (event->created() > m_syncedDateTime) {
            event->setCreated(m_syncedDateTime.addSecs(-1));
        }
        // Don't allow the event last modified time to fall after the sync time
        if (event->lastModified() > m_syncedDateTime) {
            event->setLastModified(event->created());
        }
    }
}

bool GoogleCalendarSyncAdaptor::isCleanSync(const QString &calendarId) const
{
    if (m_serverCalendarIdToCalendarInfo.contains(calendarId)) {
        return (m_serverCalendarIdToCalendarInfo[calendarId].change == GoogleCalendarSyncAdaptor::CleanSync);
    } else {
        return false;
    }
}

QJsonObject GoogleCalendarSyncAdaptor::kCalToJson(KCalendarCore::Event::Ptr event, KCalendarCore::ICalFormat &icalFormat, bool setUidProperty) const
{
    QString eventId = gCalEventId(event);
    QJsonObject start, end, originalStartTime;
    QString recurrenceId;

    QJsonArray attendees;
    const KCalendarCore::Attendee::List attendeesList = event->attendees();
    if (!attendeesList.isEmpty()) {
        const QString &organizerEmail = event->organizer().email();
        Q_FOREACH (auto att, attendeesList) {
            if (att.email().isEmpty() || att.email() == organizerEmail) {
                continue;
            }
            QJsonObject attendee;
            attendee.insert("email", att.email());
            if (att.role() == KCalendarCore::Attendee::OptParticipant) {
                attendee.insert("optional", true);
            }
            const QString &name = att.name();
            if (!name.isEmpty()) {
                attendee.insert("displayName", name);
            }
            attendees.append(attendee);
        }
    }
    // insert the date/time and timeZone information into the Json object.
    // note that timeZone is required for recurring events, for some reason.
    if (event->dtStart().time().isNull() || (event->allDay() && event->dtStart().time() == QTime(0,0,0))) {
        start.insert(QLatin1String("date"), QLocale::c().toString(event->dtStart().date(), QDATEONLY_FORMAT));
    } else {
        start.insert(QLatin1String("dateTime"), event->dtStart().toString(Qt::ISODate));
        start.insert(QLatin1String("timeZone"), QJsonValue(QString::fromUtf8(event->dtStart().timeZone().id())));
    }
    if (event->dtEnd().time().isNull() || (event->allDay() && event->dtEnd().time() == QTime(0,0,0))) {
        // For all day events, the end date is exclusive, so we need to add 1
        end.insert(QLatin1String("date"), QLocale::c().toString(event->dateEnd().addDays(1), QDATEONLY_FORMAT));
    } else {
        end.insert(QLatin1String("dateTime"), event->dtEnd().toString(Qt::ISODate));
        end.insert(QLatin1String("timeZone"), QJsonValue(QString::fromUtf8(event->dtEnd().timeZone().id())));
    }

    if (event->hasRecurrenceId()) {
        // Kcal recurrence events share their parent's id, whereas Google gives them their own id
        // and stores the parent's id in the recurrenceId field. So we must find the parent's gCalId
        KCalendarCore::Event::Ptr parent = m_calendar->event(event->uid());
        if (parent) {
            recurrenceId = gCalEventId(parent);
        } else {
            recurrenceId = eventId.contains('_') ? eventId.left(eventId.indexOf("_")) : eventId;
            qCDebug(lcSocialPlugin) << "Guessing recurrence gCalId" << recurrenceId << "from gCalId" << eventId;
        }
        originalStartTime.insert(QLatin1String("dateTime"), event->recurrenceId().toString(Qt::ISODate));
        originalStartTime.insert(QLatin1String("timeZone"), QJsonValue(QString::fromUtf8(event->recurrenceId().timeZone().id())));
    }

    QJsonObject retn;
    if (!eventId.isEmpty() && (eventId != recurrenceId)) {
        retn.insert(QLatin1String("id"), eventId);
    }
    if (event->recurrence()) {
        const QList<QDateTime> exceptions = getExceptionInstanceDates(event);
        QJsonArray recArray = recurrenceArray(event, icalFormat, exceptions);
        if (recArray.size()) {
            retn.insert(QLatin1String("recurrence"), recArray);
        }
    }
    retn.insert(QLatin1String("summary"), event->summary());
    retn.insert(QLatin1String("description"), event->description());
    retn.insert(QLatin1String("location"), event->location());
    retn.insert(QLatin1String("start"), start);
    retn.insert(QLatin1String("end"), end);
    retn.insert(QLatin1String("sequence"), QString::number(event->revision()+1));
    if (!attendees.isEmpty()) {
        retn.insert(QLatin1String("attendees"), attendees);
    }
    if (!originalStartTime.isEmpty()) {
        retn.insert(QLatin1String("recurringEventId"), recurrenceId);
        retn.insert(QLatin1String("originalStartTime"), originalStartTime);
    }
    //retn.insert(QLatin1String("locked"), event->readOnly()); // only allow locking server-side.
    // we may wish to support locking/readonly from local side also, in the future.

    // if the event has no alarms associated with it, don't let Google add one automatically
    // otherwise, attempt to upsync the alarms as popup reminders.
    QJsonObject reminders;
    if (event->alarms().count()) {
        QJsonArray overrides;
        KCalendarCore::Alarm::List alarms = event->alarms();
        for (int i = 0; i < alarms.count(); ++i) {
            // only upsync non-procedure alarms as popup reminders.
            QSet<int> seenMinutes;
            if (alarms.at(i)->type() != KCalendarCore::Alarm::Procedure) {
                const int minutes = (alarms.at(i)->startOffset().asSeconds() / 60) * -1;
                if (!seenMinutes.contains(minutes)) {
                    QJsonObject override;
                    override.insert(QLatin1String("method"), QLatin1String("popup"));
                    override.insert(QLatin1String("minutes"), minutes);
                    overrides.append(override);
                    seenMinutes.insert(minutes);
                }
            }
        }
        reminders.insert(QLatin1String("overrides"), overrides);
    }
    reminders.insert(QLatin1String("useDefault"), false);
    retn.insert(QLatin1String("reminders"), reminders);

    if (setUidProperty) {
        // now we store private extended properties: local uid.
        // this allows us to detect partially-upsynced artifacts during subsequent syncs.
        // usually this codepath will be hit for localAdditions being upsynced,
        // but sometimes also if we need to update the mapping due to clean-sync.
        QJsonObject privateExtendedProperties;
        privateExtendedProperties.insert(QLatin1String("x-jolla-sociald-mkcal-uid"), event->uid());
        QJsonObject extendedProperties;
        extendedProperties.insert(QLatin1String("private"), privateExtendedProperties);
        retn.insert(QLatin1String("extendedProperties"), extendedProperties);
    }

    return retn;
}

void GoogleCalendarSyncAdaptor::flagUploadFailure(const QString &kcalEventId)
{
    qCDebug(lcSocialPlugin) << "Setting upsync failure flag for:" << kcalEventId;
    m_eventSyncFlags.insert(kcalEventId, UploadFailure);
}

void GoogleCalendarSyncAdaptor::flagUploadSuccess(const QString &kcalEventId)
{
    // Errors take precedence
    if (!m_eventSyncFlags.contains(kcalEventId)) {
        qCDebug(lcSocialPlugin) << "Setting upsync success flag for:" << kcalEventId;
        m_eventSyncFlags.insert(kcalEventId, NoSyncFailure);
    }
}

void GoogleCalendarSyncAdaptor::flagUpdateSuccess(const QString &kcalEventId)
{
    // Errors take precedence
    if (!m_eventSyncFlags.contains(kcalEventId)) {
        qCDebug(lcSocialPlugin) << "Setting update success flag for:" << kcalEventId;
        m_eventSyncFlags.insert(kcalEventId, NoSyncFailure);
    }
}

void GoogleCalendarSyncAdaptor::flagDeleteFailure(const QString &kcalEventId)
{
    qCDebug(lcSocialPlugin) << "Setting delete failure flag for:" << kcalEventId;
    m_eventSyncFlags.insert(kcalEventId, DeleteFailure);
}

void GoogleCalendarSyncAdaptor::applySyncFailureFlag(KCalendarCore::Event::Ptr event, SyncFailure flag)
{
    const QString current = event->customProperty(VOLATILE_APP, VOLATILE_NAME);
    QString updated;

    switch (flag) {
    case UploadFailure:
        updated = QStringLiteral("upload");
        break;
    case UpdateFailure:
        updated = QStringLiteral("update");
        break;
    case DeleteFailure:
        updated = QStringLiteral("delete");
        break;
    case NoSyncFailure:
    default:
        updated = QString();
        break;
    }

    if (current != updated) {
        qCDebug(lcSocialPlugin) << "Changing flag from" << current << "to" << updated << "for" << event->uid();
        if (!updated.isEmpty()) {
            event->setCustomProperty(VOLATILE_APP, VOLATILE_NAME, updated);

        } else {
            event->removeCustomProperty(VOLATILE_APP, VOLATILE_NAME);
        }
        m_storageNeedsSave = true;
    }
}

void GoogleCalendarSyncAdaptor::applySyncFailureFlags()
{
    qCDebug(lcSocialPlugin) << "Applying sync failure flags for calendar";
    // Iterate over the incidences with flags associated with them
    QMap<QString, SyncFailure>::const_iterator iter = m_eventSyncFlags.constBegin();
    while (iter != m_eventSyncFlags.constEnd()) {
        const QString &uid = iter.key();
        const SyncFailure flag = iter.value();

        KCalendarCore::Event::Ptr event = m_calendar->event(uid);
        if (!event) {
            // Load it if it wasn't already
            m_storage->loadSeries(uid);
            event = m_calendar->event(uid);
        }

        // Find the parent event
        if (event) {
            applySyncFailureFlag(event, flag);
            // Iterate over the exception instances of the parent
            const KCalendarCore::Event::List instances = m_calendar->eventInstances(event);
            for (KCalendarCore::Event::Ptr instance : instances) {
                applySyncFailureFlag(instance, flag);
            }
        }
        ++iter;
    }
}

