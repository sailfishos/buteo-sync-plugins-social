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

#include "onedrivedatatypesyncadaptor.h"
#include "trace.h"

#include <QtCore/QVariantMap>
#include <QtCore/QObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QByteArray>

//libsailfishkeyprovider
#include <sailfishkeyprovider.h>

// libaccounts-qt5
#include <Accounts/Manager>
#include <Accounts/Account>
#include <Accounts/Service>
#include <Accounts/AccountService>

//libsignon-qt: SignOn::NoUserInteractionPolicy
#include <SignOn/Identity>
#include <SignOn/AuthSession>
#include <SignOn/SessionData>

OneDriveDataTypeSyncAdaptor::OneDriveDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::DataType dataType, QObject *parent)
    : SocialNetworkSyncAdaptor("onedrive", dataType, 0, parent), m_triedLoading(false)
{
}

OneDriveDataTypeSyncAdaptor::~OneDriveDataTypeSyncAdaptor()
{
}

void OneDriveDataTypeSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    if (dataTypeString != SocialNetworkSyncAdaptor::dataTypeName(m_dataType)) {
        qCWarning(lcSocialPlugin) << "OneDrive" << SocialNetworkSyncAdaptor::dataTypeName(m_dataType)
                                  << "sync adaptor was asked to sync" << dataTypeString;
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    if (clientId().isEmpty()) {
        qCWarning(lcSocialPlugin) << "client id couldn't be retrieved for OneDrive account" << accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    setStatus(SocialNetworkSyncAdaptor::Busy);
    updateDataForAccount(accountId);
    qCDebug(lcSocialPlugin) << "successfully triggered sync with profile:" << m_accountSyncProfile->name();
}

void OneDriveDataTypeSyncAdaptor::updateDataForAccount(int accountId)
{
    Accounts::Account *account = Accounts::Account::fromId(m_accountManager, accountId, this);
    if (!account) {
        qCWarning(lcSocialPlugin) << "existing account with id" << accountId << "couldn't be retrieved";
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    // will be decremented by either signOnError or signOnResponse.
    incrementSemaphore(accountId);
    signIn(account);
}

void OneDriveDataTypeSyncAdaptor::finalCleanup()
{
}

void OneDriveDataTypeSyncAdaptor::errorHandler(QNetworkReply::NetworkError err)
{
    // OneDrive sends error code 204 (HTTP code 401) for Unauthorized Error
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    const int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (err == QNetworkReply::AuthenticationRequiredError) {
        int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        qCInfo(lcSocialPlugin) << "sociald:OneDrive: received:" << httpCode
                               << "would normally set CredentialsNeedUpdate for account"
                               << reply->property("accountId").toInt()
                               << "but could be spurious";
    }

    qCWarning(lcSocialPlugin) << SocialNetworkSyncAdaptor::dataTypeName(m_dataType)
                              << "request with account" << sender()->property("accountId").toInt()
                              << "experienced error:" << err << "HTTP code:" << httpCode
                              << "data:" << reply->readAll();
    // set "isError" on the reply so that adapters know to ignore the result in the finished() handler
    reply->setProperty("isError", QVariant::fromValue<bool>(true));
    // Note: not all errors are "unrecoverable" errors, so we don't change the status here.
}

void OneDriveDataTypeSyncAdaptor::sslErrorsHandler(const QList<QSslError> &errs)
{
    QString sslerrs;
    foreach (const QSslError &e, errs) {
        sslerrs += e.errorString() + "; ";
    }
    if (errs.size() > 0) {
        sslerrs.chop(2);
    }
    qCWarning(lcSocialPlugin) << SocialNetworkSyncAdaptor::dataTypeName(m_dataType)
                              << "request with account" << sender()->property("accountId").toInt()
                              << "experienced ssl errors:" << sslerrs;
    // set "isError" on the reply so that adapters know to ignore the result in the finished() handler
    sender()->setProperty("isError", QVariant::fromValue<bool>(true));
    // Note: not all errors are "unrecoverable" errors, so we don't change the status here.
}

QString OneDriveDataTypeSyncAdaptor::api() const
{
    return m_api;
}

QString OneDriveDataTypeSyncAdaptor::clientId()
{
    if (!m_triedLoading) {
        loadClientId();
    }
    return m_clientId;
}

void OneDriveDataTypeSyncAdaptor::loadClientId()
{
    m_triedLoading = true;
    char *cClientId = NULL;

    int cSuccess = SailfishKeyProvider_storedKey("onedrive", "onedrive-sync", "client_id", &cClientId);
    if (cClientId == NULL) {
        return;
    } else if (cSuccess != 0) {
        free(cClientId);
        return;
    }

    m_clientId = QLatin1String(cClientId);
    free(cClientId);
}

void OneDriveDataTypeSyncAdaptor::setCredentialsNeedUpdate(Accounts::Account *account)
{
    qWarning() << "sociald:OneDrive: setting CredentialsNeedUpdate to true for account:" << account->id();
    Accounts::Service srv(m_accountManager->service(syncServiceName()));
    account->selectService(srv);
    account->setValue(QStringLiteral("CredentialsNeedUpdate"), QVariant::fromValue<bool>(true));
    account->setValue(QStringLiteral("CredentialsNeedUpdateFrom"), QVariant::fromValue<QString>(QString::fromLatin1("sociald-onedrive")));
    account->selectService(Accounts::Service());
    account->syncAndBlock();
}

void OneDriveDataTypeSyncAdaptor::signIn(Accounts::Account *account)
{
    // Fetch consumer key from keyprovider
    int accountId = account->id();
    if (!checkAccount(account) || clientId().isEmpty()) {
        decrementSemaphore(accountId);
        return;
    }

    // grab out a valid identity for the sync service.
    Accounts::Service srv(m_accountManager->service(syncServiceName()));
    account->selectService(srv);
    SignOn::Identity *identity = account->credentialsId() > 0 ? SignOn::Identity::existingIdentity(account->credentialsId()) : 0;
    if (!identity) {
        qCWarning(lcSocialPlugin) << "account" << accountId << "has no valid credentials; cannot sign in";
        decrementSemaphore(accountId);
        return;
    }

    Accounts::AccountService accSrv(account, srv);
    QString method = accSrv.authData().method();
    QString mechanism = accSrv.authData().mechanism();
    SignOn::AuthSession *session = identity->createSession(method);
    if (!session) {
        qCWarning(lcSocialPlugin) << "could not create signon session for account" << accountId;
        identity->deleteLater();
        decrementSemaphore(accountId);
        return;
    }

    QVariantMap signonSessionData = accSrv.authData().parameters();
    signonSessionData.insert("ClientId", clientId());
    signonSessionData.insert("UiPolicy", SignOn::NoUserInteractionPolicy);

    connect(session, SIGNAL(response(SignOn::SessionData)),
            this, SLOT(signOnResponse(SignOn::SessionData)),
            Qt::UniqueConnection);
    connect(session, SIGNAL(error(SignOn::Error)),
            this, SLOT(signOnError(SignOn::Error)),
            Qt::UniqueConnection);

    session->setProperty("account", QVariant::fromValue<Accounts::Account*>(account));
    session->setProperty("identity", QVariant::fromValue<SignOn::Identity*>(identity));
    session->process(SignOn::SessionData(signonSessionData), mechanism);
}

void OneDriveDataTypeSyncAdaptor::signOnError(const SignOn::Error &error)
{
    SignOn::AuthSession *session = qobject_cast<SignOn::AuthSession*>(sender());
    Accounts::Account *account = session->property("account").value<Accounts::Account*>();
    SignOn::Identity *identity = session->property("identity").value<SignOn::Identity*>();
    int accountId = account->id();
    qCWarning(lcSocialPlugin) << "credentials for account with id" << accountId
                              << "couldn't be retrieved:" << error.type() << error.message();

    // if the error is because credentials have expired, we
    // set the CredentialsNeedUpdate key.
    if (error.type() == SignOn::Error::UserInteraction) {
        setCredentialsNeedUpdate(account);
    }

    session->disconnect(this);
    identity->destroySession(session);
    identity->deleteLater();
    account->deleteLater();

    // if we couldn't sign in, we can't sync with this account.
    setStatus(SocialNetworkSyncAdaptor::Error);
    decrementSemaphore(accountId);
}

void OneDriveDataTypeSyncAdaptor::signOnResponse(const SignOn::SessionData &responseData)
{
    QVariantMap data;
    foreach (const QString &key, responseData.propertyNames()) {
        data.insert(key, responseData.getProperty(key));
    }

    QString accessToken;
    SignOn::AuthSession *session = qobject_cast<SignOn::AuthSession*>(sender());
    Accounts::Account *account = session->property("account").value<Accounts::Account*>();
    SignOn::Identity *identity = session->property("identity").value<SignOn::Identity*>();
    int accountId = account->id();
    if (data.contains(QLatin1String("AccessToken"))) {
        accessToken = data.value(QLatin1String("AccessToken")).toString();
    } else {
        qCInfo(lcSocialPlugin) << "signon response for account with id" << accountId << "contained no access token";
    }

    m_api = account->value(QStringLiteral("api/Host")).toString();

    session->disconnect(this);
    identity->destroySession(session);
    identity->deleteLater();
    account->deleteLater();

    if (!accessToken.isEmpty()) {
        beginSync(accountId, accessToken); // call the derived-class sync entrypoint.
    }

    decrementSemaphore(accountId);
}
