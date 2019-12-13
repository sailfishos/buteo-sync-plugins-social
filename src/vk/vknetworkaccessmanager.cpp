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

#include "vknetworkaccessmanager_p.h"
#include "trace.h"

#include <QDateTime>
#include <QString>
#include <QObject>
#include <QNetworkReply>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <utime.h>

namespace {
    bool touchTimestampFile()
    {
        static const QString timestampFileName = QString::fromLatin1("%1/%2/vktimestamp")
                .arg(PRIVILEGED_DATA_DIR)
                .arg(QString::fromLatin1(SYNC_DATABASE_DIR));
        QByteArray tsfnba = timestampFileName.toUtf8();

        int fd = open(tsfnba.constData(), O_WRONLY|O_CREAT|O_NOCTTY|O_NONBLOCK, 0666);
        if (fd < 0) {
            return false;
        }

        int rv = utimensat(AT_FDCWD, tsfnba.constData(), 0, 0);
        close(fd);
        if (rv != 0) {
            return false;
        }

        return true;
    }

    qint64 readTimestampFile()
    {
        static const QString timestampFileName = QString::fromLatin1("%1/%2/vktimestamp")
                .arg(PRIVILEGED_DATA_DIR)
                .arg(QString::fromLatin1(SYNC_DATABASE_DIR));
        QByteArray tsfnba = timestampFileName.toUtf8();

        struct stat buf;
        if (stat(tsfnba.constData(), &buf) < 0) {
            return 0;
        }

        time_t tvsec = buf.st_mtim.tv_sec;
        long nanosec = buf.st_mtim.tv_nsec;
        qint64 msecs = (tvsec*1000) + (nanosec/1000000);
        return msecs;
    }
}

VKNetworkAccessManager::VKNetworkAccessManager(QObject *parent)
    : SocialdNetworkAccessManager(parent)
{
}

QNetworkReply *VKNetworkAccessManager::createRequest(
                                 QNetworkAccessManager::Operation op,
                                 const QNetworkRequest &req,
                                 QIODevice *outgoingData)
{
    // VK throttles requests.  We want to wait at least 550 ms between each request.
    // To do this properly, we need to protect the file access with a semaphore
    // or link-lock to prevent concurrent process access.  For now, we use the
    // naive approach.

    qint64 currTime = QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
    qint64 lastRequestTime = readTimestampFile();
    qint64 delta = currTime - lastRequestTime;
    if (lastRequestTime == 0 || delta > VK_THROTTLE_INTERVAL) {
        touchTimestampFile();
        return SocialdNetworkAccessManager::createRequest(op, req, outgoingData);
    }

    SOCIALD_LOG_DEBUG("Throttling request! lastRequestTime:" << lastRequestTime << ", currTime:" << currTime << ", so delta:" << delta);
    return 0; // tell the client to resubmit their request, it was throttled.
}
