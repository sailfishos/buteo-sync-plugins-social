/****************************************************************************
 **
 ** Copyright (C) 2019 Open Mobile Platform LLC
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

#include "backuprestoreoptions_p.h"
#include "trace.h"

#include <QtCore/QCryptographicHash>

// nemo
#include <ssudeviceinfo.h>

// buteo-syncfw
#include <SyncProfile.h>

bool BackupRestoreOptions::copyToProfile(Buteo::SyncProfile *syncProfile)
{
    if (!syncProfile) {
        qWarning() << "Invalid profile!";
        return false;
    }

    Buteo::Profile *clientProfile = syncProfile->clientProfile();
    if (!clientProfile) {
        qWarning() << "Cannot find client profile in sync profile:" << syncProfile->name();
        return false;
    }

    clientProfile->setKey("sfos-dir-local", localDirPath);
    clientProfile->setKey("sfos-dir-remote", remoteDirPath);
    clientProfile->setKey("sfos-filename", fileName);

    return true;
}

BackupRestoreOptions BackupRestoreOptions::fromProfile(Buteo::SyncProfile *syncProfile, bool *ok)
{
    if (!syncProfile) {
        qWarning() << "Invalid sync profile!";
        return BackupRestoreOptions();
    }

    Buteo::Profile *clientProfile = syncProfile->clientProfile();
    if (!clientProfile) {
        qWarning() << "Cannot find client profile in sync profile:" << syncProfile->name();
        return BackupRestoreOptions();
    }

    BackupRestoreOptions options;
    options.localDirPath = clientProfile->key("sfos-dir-local");
    options.remoteDirPath = clientProfile->key("sfos-dir-remote");
    options.fileName = clientProfile->key("sfos-filename");

    if (options.localDirPath.isEmpty()) {
        qWarning() << "Backup/restore options for sync profile" << syncProfile->name()
                   << "do not specify a local directory!";
        return BackupRestoreOptions();
    }

    if (ok) {
        *ok = true;
    }
    return options;
}

QString BackupRestoreOptions::backupDeviceName()
{
    SsuDeviceInfo deviceInfo;
    const QString deviceId = deviceInfo.deviceUid();
    const QByteArray hashedDeviceId = QCryptographicHash::hash(deviceId.toUtf8(), QCryptographicHash::Sha256);
    const QString encodedDeviceId = QString::fromUtf8(hashedDeviceId.toBase64(QByteArray::Base64UrlEncoding)).mid(0,12);
    if (deviceId.isEmpty()) {
        qWarning() << "Could not determine device identifier for backup directory name!";
        return QString();
    }

    QString deviceDisplayNamePrefix = deviceInfo.displayName(Ssu::DeviceModel);
    if (!deviceDisplayNamePrefix.isEmpty()) {
        deviceDisplayNamePrefix = deviceDisplayNamePrefix.replace(' ', '-') + '_';
    }

    return deviceDisplayNamePrefix + encodedDeviceId;
}
