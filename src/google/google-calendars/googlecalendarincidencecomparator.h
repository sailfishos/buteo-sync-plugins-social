/****************************************************************************
 **
 ** Copyright (C) 2015 Jolla Ltd.
 ** Contact: Chris Adams <chris.adams@jollamobile.com>
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

#ifndef GOOGLECALENDARINCIDENCECOMPARATOR_H
#define GOOGLECALENDARINCIDENCECOMPARATOR_H

#include <QtDebug>

#include <extendedcalendar.h>
#include <extendedstorage.h>

#include <KCalendarCore/ICalFormat>
#include <KCalendarCore/MemoryCalendar>
#include <KCalendarCore/Incidence>
#include <KCalendarCore/Event>
#include <KCalendarCore/Todo>
#include <KCalendarCore/Journal>
#include <KCalendarCore/Attendee>

#include "trace.h"

#define GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, func, desc) {\
    if (a->func != b->func) {\
        qCDebug(lcSocialPlugin) << "Incidence" << desc << "" << "properties are not equal:" << a->func << b->func; \
        return false;\
    }\
}

#define GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(failureCheck, desc, debug) {\
    if (failureCheck) {\
        qCDebug(lcSocialPlugin) << "Incidence" << desc << "properties are not equal:" << desc << debug; \
        return false;\
    }\
}

namespace GoogleCalendarIncidenceComparator {
    void normalizePersonEmail(KCalendarCore::Person *p)
    {
        QString email = p->email().replace(QStringLiteral("mailto:"), QString(), Qt::CaseInsensitive);
        if (email != p->email()) {
            p->setEmail(email);
        }
    }

    template <typename T>
    bool pointerDataEqual(const QVector<QSharedPointer<T> > &vectorA, const QVector<QSharedPointer<T> > &vectorB)
    {
        if (vectorA.count() != vectorB.count()) {
            return false;
        }
        for (int i=0; i<vectorA.count(); i++) {
            if (vectorA[i].data() != vectorB[i].data()) {
                return false;
            }
        }
        return true;
    }

    bool eventsEqual(const KCalendarCore::Event::Ptr &a, const KCalendarCore::Event::Ptr &b)
    {
        GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(a->dateEnd() != b->dateEnd(), "dateEnd", (a->dateEnd().toString() + " != " + b->dateEnd().toString()));
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, transparency(), "transparency");

        // some special handling for dtEnd() depending on whether it's an all-day event or not.
        if (a->allDay() && b->allDay()) {
            GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(a->dtEnd().date() != b->dtEnd().date(), "dtEnd", (a->dtEnd().toString() + " != " + b->dtEnd().toString()));
        } else {
            GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(a->dtEnd() != b->dtEnd(), "dtEnd", (a->dtEnd().toString() + " != " + b->dtEnd().toString()));
        }

        // some special handling for isMultiday() depending on whether it's an all-day event or not.
        if (a->allDay() && b->allDay()) {
            // here we assume that both events are in "export form" (that is, exclusive DTEND)
            if (a->dtEnd().date() != b->dtEnd().date()) {
                qCDebug(lcSocialPlugin) << "have a->dtStart()" << a->dtStart().toString() << ", a->dtEnd()" << a->dtEnd().toString();
                qCDebug(lcSocialPlugin) << "have b->dtStart()" << b->dtStart().toString() << ", b->dtEnd()" << b->dtEnd().toString();
                qCDebug(lcSocialPlugin) << "have a->isMultiDay()" << a->isMultiDay() << ", b->isMultiDay()" << b->isMultiDay();
                return false;
            }
        } else {
            GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, isMultiDay(), "multiday");
        }

        // Don't compare hasEndDate() as Event(Event*) does not initialize it based on the validity of
        // dtEnd(), so it could be false when dtEnd() is valid. The dtEnd comparison above is sufficient.

        return true;
    }

    bool todosEqual(const KCalendarCore::Todo::Ptr &a, const KCalendarCore::Todo::Ptr &b)
    {
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, hasCompletedDate(), "hasCompletedDate");
        GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(a->dtRecurrence() != b->dtRecurrence(), "dtRecurrence", (a->dtRecurrence().toString() + " != " + b->dtRecurrence().toString()));
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, hasDueDate(), "hasDueDate");
        GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(a->dtDue() != b->dtDue(), "dtDue", (a->dtDue().toString() + " != " + b->dtDue().toString()));
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, hasStartDate(), "hasStartDate");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, isCompleted(), "isCompleted");
        GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(a->completed() != b->completed(), "completed", (a->completed().toString() + " != " + b->completed().toString()));
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, isOpenEnded(), "isOpenEnded");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, percentComplete(), "percentComplete");
        return true;
    }

    bool journalsEqual(const KCalendarCore::Journal::Ptr &, const KCalendarCore::Journal::Ptr &)
    {
        // no journal-specific properties; it only uses the base incidence properties
        return true;
    }

    // Checks whether a specific set of properties are equal.
    bool incidencesEqual(const KCalendarCore::Incidence::Ptr &a, const KCalendarCore::Incidence::Ptr &b)
    {
        if (!a || !b) {
            qWarning() << "Invalid parameters! a:" << a << "b:" << b;
            return false;
        }

        // Do not compare created() or lastModified() because we don't update these fields when
        // an incidence is updated by copyIncidenceProperties(), so they are guaranteed to be unequal.
        // TODO compare deref alarms and attachment lists to compare them also.
        // Don't compare resources() for now because KCalendarCore may insert QStringList("") as the resources
        // when in fact it should be QStringList(), which causes the comparison to fail.
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, type(), "type");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, duration(), "duration");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, hasDuration(), "hasDuration");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, isReadOnly(), "isReadOnly");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, comments(), "comments");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, contacts(), "contacts");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, altDescription(), "altDescription");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, categories(), "categories");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, customStatus(), "customStatus");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, description(), "description");
        GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(!qFuzzyCompare(a->geoLatitude(), b->geoLatitude()), "geoLatitude", (QString("%1 != %2").arg(a->geoLatitude()).arg(b->geoLatitude())));
        GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(!qFuzzyCompare(a->geoLongitude(), b->geoLongitude()), "geoLongitude", (QString("%1 != %2").arg(a->geoLongitude()).arg(b->geoLongitude())));
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, hasGeo(), "hasGeo");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, location(), "location");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, secrecy(), "secrecy");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, status(), "status");
        GIC_RETURN_FALSE_IF_NOT_EQUAL(a, b, summary(), "summary");

        // check recurrence information. Note that we only need to check the recurrence rules for equality if they both recur.
        GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(a->recurs() != b->recurs(), "recurs", a->recurs() + " != " + b->recurs());
        GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(a->recurs() && *(a->recurrence()) != *(b->recurrence()), "recurrence", "...");

        // some special handling for dtStart() depending on whether it's an all-day event or not.
        if (a->allDay() && b->allDay()) {
            GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(a->dtStart().date() != b->dtStart().date(), "dtStart", (a->dtStart().toString() + " != " + b->dtStart().toString()));
        } else {
            GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(a->dtStart() != b->dtStart(), "dtStart", (a->dtStart().toString() + " != " + b->dtStart().toString()));
        }

        // Some servers insert a mailto: in the organizer email address, so ignore this when comparing organizers
        KCalendarCore::Person personA(a->organizer());
        KCalendarCore::Person personB(b->organizer());
        normalizePersonEmail(&personA);
        normalizePersonEmail(&personB);
        const QString aEmail = personA.email();
        const QString bEmail = personB.email();
        // If the aEmail is empty, the local event doesn't have organizer info.
        // That's ok - Google will add organizer/creator info when we upsync,
        // so don't treat it as a local modification.
        // Otherwise, it is a "real" change.
        if (aEmail != bEmail && !aEmail.isEmpty()) {
            GIC_RETURN_FALSE_IF_NOT_EQUAL_CUSTOM(personA != personB, "organizer", (personA.fullName() + " != " + personB.fullName()));
        }

        switch (a->type()) {
        case KCalendarCore::IncidenceBase::TypeEvent:
            if (!eventsEqual(a.staticCast<KCalendarCore::Event>(), b.staticCast<KCalendarCore::Event>())) {
                return false;
            }
            break;
        case KCalendarCore::IncidenceBase::TypeTodo:
            if (!todosEqual(a.staticCast<KCalendarCore::Todo>(), b.staticCast<KCalendarCore::Todo>())) {
                return false;
            }
            break;
        case KCalendarCore::IncidenceBase::TypeJournal:
            if (!journalsEqual(a.staticCast<KCalendarCore::Journal>(), b.staticCast<KCalendarCore::Journal>())) {
                return false;
            }
            break;
        case KCalendarCore::IncidenceBase::TypeFreeBusy:
        case KCalendarCore::IncidenceBase::TypeUnknown:
            qCDebug(lcSocialPlugin) << "Unable to compare FreeBusy or Unknown incidence, assuming equal";
            break;
        }
        return true;
    }
}

#endif // GOOGLECALENDARINCIDENCECOMPARATOR_H
