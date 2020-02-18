TARGET = onedrive-backuprestore-client

DEFINES += "CLASSNAME=OneDriveBackupRestorePlugin"
DEFINES += CLASSNAME_H=\\\"onedrivebackuprestoreplugin.h\\\"
include($$PWD/../../common.pri)
include($$PWD/../onedrive-common.pri)
include($$PWD/../onedrive-backupoperation.pri)
include($$PWD/onedrive-backuprestore.pri)

onedrive_backuprestore_sync_profile.path = /etc/buteo/profiles/sync
onedrive_backuprestore_sync_profile.files = $$PWD/onedrive.BackupRestore.xml
onedrive_backuprestore_client_plugin_xml.path = /etc/buteo/profiles/client
onedrive_backuprestore_client_plugin_xml.files = $$PWD/onedrive-backuprestore.xml

HEADERS += onedrivebackuprestoreplugin.h
SOURCES += onedrivebackuprestoreplugin.cpp

OTHER_FILES += \
    onedrive_backuprestore_sync_profile.files \
    onedrive_backuprestore_client_plugin_xml.files

INSTALLS += \
    target \
    onedrive_backuprestore_sync_profile \
    onedrive_backuprestore_client_plugin_xml
