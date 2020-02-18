TARGET = dropbox-backuprestore-client

DEFINES += "CLASSNAME=DropboxBackupRestorePlugin"
DEFINES += CLASSNAME_H=\\\"dropboxbackuprestoreplugin.h\\\"
include($$PWD/../../common.pri)
include($$PWD/../dropbox-common.pri)
include($$PWD/../dropbox-backupoperation.pri)
include($$PWD/dropbox-backuprestore.pri)

dropbox_backuprestore_sync_profile.path = /etc/buteo/profiles/sync
dropbox_backuprestore_sync_profile.files = $$PWD/dropbox.BackupRestore.xml
dropbox_backuprestore_client_plugin_xml.path = /etc/buteo/profiles/client
dropbox_backuprestore_client_plugin_xml.files = $$PWD/dropbox-backuprestore.xml

HEADERS += dropboxbackuprestoreplugin.h
SOURCES += dropboxbackuprestoreplugin.cpp

OTHER_FILES += \
    dropbox_backuprestore_sync_profile.files \
    dropbox_backuprestore_client_plugin_xml.files

INSTALLS += \
    target \
    dropbox_backuprestore_sync_profile \
    dropbox_backuprestore_client_plugin_xml
