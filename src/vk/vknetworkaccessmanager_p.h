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

#ifndef SOCIALD_VK_QNAMFACTORY_P_H
#define SOCIALD_VK_QNAMFACTORY_P_H

#include "socialdnetworkaccessmanager_p.h"

#define VK_THROTTLE_INTERVAL 550 /* msec */
#define VK_THROTTLE_EXTRA_INTERVAL 3000 /* msec */
#define VK_THROTTLE_ERROR_CODE 6
#define VK_THROTTLE_RETRY_LIMIT 30

class VKNetworkAccessManager : public SocialdNetworkAccessManager
{
    Q_OBJECT

public:
    VKNetworkAccessManager(QObject *parent = 0);

protected:
    QNetworkReply *createRequest(QNetworkAccessManager::Operation op,
                                 const QNetworkRequest &req,
                                 QIODevice *outgoingData = 0) override;
};

#endif // SOCIALD_VK_QNAMFACTORY_P_H
