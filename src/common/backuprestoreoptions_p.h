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

#ifndef BACKUPRESTOREOPTIONS_H
#define BACKUPRESTOREOPTIONS_H

#include <QtCore/QString>

namespace Buteo {
    class SyncProfile;
}

// Basically a mirror of AccountSyncManager::BackupRestoreOptions.
class BackupRestoreOptions
{
public:
    QString localDirPath;
    QString remoteDirPath;
    QString fileName;

    bool copyToProfile(Buteo::SyncProfile *syncProfile);

    static BackupRestoreOptions fromProfile(Buteo::SyncProfile *syncProfile, bool *ok);

    static QString backupDeviceName();
};

#endif // BACKUPRESTOREOPTIONS_H
