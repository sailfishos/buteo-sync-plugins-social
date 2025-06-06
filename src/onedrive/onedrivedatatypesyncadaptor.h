/****************************************************************************
 **
 ** Copyright (c) 2015 - 2019 Jolla Ltd.
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

#ifndef ONEDRIVEDATATYPESYNCADAPTOR_H
#define ONEDRIVEDATATYPESYNCADAPTOR_H

#include "socialnetworksyncadaptor.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslError>
#include <QtCore/QJsonObject>

namespace Accounts {
    class Account;
}
namespace SignOn {
    class Error;
    class SessionData;
}

/*
    Abstract interface for all of the data-specific sync adaptors
    which pull data from OneDrive's online services.
*/

class OneDriveDataTypeSyncAdaptor : public SocialNetworkSyncAdaptor
{
    Q_OBJECT

public:
    OneDriveDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::DataType dataType, QObject *parent);
    virtual ~OneDriveDataTypeSyncAdaptor();

    virtual void sync(const QString &dataTypeString, int accountId);

protected:
    QString api() const;
    QString clientId();
    virtual void updateDataForAccount(int accountId);
    virtual void beginSync(int accountId, const QString &accessToken) = 0;
    virtual void finalCleanup();    

protected Q_SLOTS:
    virtual void errorHandler(QNetworkReply::NetworkError err);
    virtual void sslErrorsHandler(const QList<QSslError> &errs);

private Q_SLOTS:
    void signOnError(const SignOn::Error &error);
    void signOnResponse(const SignOn::SessionData &responseData);

private:
    void loadClientId();
    void setCredentialsNeedUpdate(Accounts::Account *account);
    void signIn(Accounts::Account *account);
    bool m_triedLoading; // Is true if we tried to load (even if we failed)
    QString m_clientId;
    QString m_api;
};

#endif // ONEDRIVEDATATYPESYNCADAPTOR_H
