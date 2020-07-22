Name:       sociald
Summary:    Syncs device data from social services
Version:    0.2.25
Release:    1
License:    LGPLv2
URL:        https://git.sailfishos.org/mer-core/buteo-sync-plugins-social
Source0:    %{name}-%{version}.tar.bz2
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5DBus)
BuildRequires:  pkgconfig(Qt5Sql)
BuildRequires:  pkgconfig(Qt5Network)
BuildRequires:  pkgconfig(mlite5)
BuildRequires:  pkgconfig(buteosyncfw5) >= 0.6.36
BuildRequires:  pkgconfig(libsignon-qt5)
BuildRequires:  pkgconfig(accounts-qt5) >= 1.13
BuildRequires:  pkgconfig(socialcache) >= 0.0.48
BuildRequires:  pkgconfig(libsailfishkeyprovider)
BuildRequires:  pkgconfig(qtcontacts-sqlite-qt5-extensions)
BuildRequires:  qt5-qttools-linguist
BuildRequires:  ssu-devel
Requires: buteo-syncfw-qt5-msyncd
Requires: systemd
Requires(pre):  sailfish-setup
Requires(post): systemd
Obsoletes: sociald-facebook-notifications

%description
A Buteo plugin which provides data synchronization with various social services.

%files
%defattr(-,root,root,-)
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/sociald-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libsociald-client.so
%config %{_sysconfdir}/buteo/profiles/client/sociald.xml
%config %{_sysconfdir}/buteo/profiles/sync/sociald.All.xml
%license COPYING

%package facebook-calendars
Summary:    Provides calendar synchronisation with Facebook
BuildRequires:  pkgconfig(libmkcal-qt5)
BuildRequires:  pkgconfig(libkcalcoren-qt5)
Requires: %{name} = %{version}-%{release}

%description facebook-calendars
%{summary}.

%files facebook-calendars
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/facebook-calendars-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libfacebook-calendars-client.so
%config %{_sysconfdir}/buteo/profiles/client/facebook-calendars.xml
%config %{_sysconfdir}/buteo/profiles/sync/facebook.Calendars.xml

%pre facebook-calendars
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/facebook-calendars.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/facebook.Calendars.xml || :
done

%post facebook-calendars
systemctl-user try-restart msyncd.service || :


%package facebook-contacts
Summary:    Provides contact synchronisation with Facebook
BuildRequires:  pkgconfig(Qt5Gui)
BuildRequires:  pkgconfig(Qt5Contacts)
BuildRequires:  pkgconfig(qtcontacts-sqlite-qt5-extensions)
Requires: %{name} = %{version}-%{release}

%description facebook-contacts
%{summary}

%files facebook-contacts
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/facebook-contacts-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libfacebook-contacts-client.so
%config %{_sysconfdir}/buteo/profiles/client/facebook-contacts.xml
%config %{_sysconfdir}/buteo/profiles/sync/facebook.Contacts.xml

%pre facebook-contacts
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/facebook-contacts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/facebook.Contacts.xml || :
done

%post facebook-contacts
systemctl-user try-restart msyncd.service || :


%package facebook-images
Summary:    Provides image synchronisation with Facebook
Requires: %{name} = %{version}-%{release}

%description facebook-images
%{summary}.

%files facebook-images
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/facebook-images-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libfacebook-images-client.so
%config %{_sysconfdir}/buteo/profiles/client/facebook-images.xml
%config %{_sysconfdir}/buteo/profiles/sync/facebook.Images.xml

%pre facebook-images
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/facebook-images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/facebook.Images.xml || :
done

%post facebook-images
systemctl-user try-restart msyncd.service || :


%package facebook-signon
Summary:    Provides signon credentials refreshing with Facebook
BuildRequires:  qt5-qttools-linguist
Requires: %{name} = %{version}-%{release}

%description facebook-signon
%{summary}.

%files facebook-signon
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/facebook-signon-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libfacebook-signon-client.so
%config %{_sysconfdir}/buteo/profiles/client/facebook-signon.xml
%config %{_sysconfdir}/buteo/profiles/sync/facebook.Signon.xml

%pre facebook-signon
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/facebook-signon.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/facebook.Signon.xml || :
done

%post facebook-signon
systemctl-user try-restart msyncd.service || :



%package google-calendars
Summary:    Provides calendar synchronisation with Google
BuildRequires:  pkgconfig(libmkcal-qt5) >= 0.5.9
BuildRequires:  pkgconfig(libkcalcoren-qt5)
Requires: %{name} = %{version}-%{release}

%description google-calendars
%{summary}.

%files google-calendars
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/google-calendars-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libgoogle-calendars-client.so
%config %{_sysconfdir}/buteo/profiles/client/google-calendars.xml
%config %{_sysconfdir}/buteo/profiles/sync/google.Calendars.xml

%pre google-calendars
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/google-calendars.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/google.Calendars.xml || :
done

%post google-calendars
systemctl-user try-restart msyncd.service || :


%package google-contacts
Summary:    Provides contact synchronisation with Google
BuildRequires:  pkgconfig(Qt5Gui)
BuildRequires:  pkgconfig(Qt5Contacts)
BuildRequires:  pkgconfig(qtcontacts-sqlite-qt5-extensions) >= 0.1.58
Requires: %{name} = %{version}-%{release}

%description google-contacts
%{summary}.

%files google-contacts
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/google-contacts-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libgoogle-contacts-client.so
%config %{_sysconfdir}/buteo/profiles/client/google-contacts.xml
%config %{_sysconfdir}/buteo/profiles/sync/google.Contacts.xml

%pre google-contacts
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/google-contacts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/google.Contacts.xml || :
done

%post google-contacts
systemctl-user try-restart msyncd.service || :


%package google-signon
Summary:    Provides signon credentials refreshing with Google
BuildRequires:  qt5-qttools-linguist
Requires: %{name} = %{version}-%{release}

%description google-signon
%{summary}.

%files google-signon
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/google-signon-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libgoogle-signon-client.so
%config %{_sysconfdir}/buteo/profiles/client/google-signon.xml
%config %{_sysconfdir}/buteo/profiles/sync/google.Signon.xml

%pre google-signon
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/google-signon.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/google.Signon.xml || :
done

%post google-signon
systemctl-user try-restart msyncd.service || :



%package twitter-notifications
Summary:    Provides notification synchronisation with Twitter
BuildRequires:  pkgconfig(Qt5Contacts)
BuildRequires:  pkgconfig(qtcontacts-sqlite-qt5-extensions)
BuildRequires:  nemo-qml-plugin-notifications-qt5-devel
BuildRequires:  qt5-qttools-linguist
Requires: %{name} = %{version}-%{release}

%description twitter-notifications
%{summary}.

%files twitter-notifications
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/twitter-notifications-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libtwitter-notifications-client.so
%config %{_sysconfdir}/buteo/profiles/client/twitter-notifications.xml
%config %{_sysconfdir}/buteo/profiles/sync/twitter.Notifications.xml
%{_datadir}/translations/lipstick-jolla-home-twitter-notif_eng_en.qm

%pre twitter-notifications
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/twitter-notifications.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/twitter.Notifications.xml || :
done

%post twitter-notifications
systemctl-user try-restart msyncd.service || :


%package twitter-posts
Summary:    Provides post synchronisation with Twitter
BuildRequires:  pkgconfig(Qt5Contacts)
BuildRequires:  pkgconfig(qtcontacts-sqlite-qt5-extensions)
BuildRequires:  qt5-qttools-linguist
Requires: %{name} = %{version}-%{release}

%description twitter-posts
%{summary}.

%files twitter-posts
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/twitter-posts-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libtwitter-posts-client.so
%config %{_sysconfdir}/buteo/profiles/client/twitter-posts.xml
%config %{_sysconfdir}/buteo/profiles/sync/twitter.Posts.xml

%pre twitter-posts
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/twitter-posts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/twitter.Posts.xml || :
done

%post twitter-posts
systemctl-user try-restart msyncd.service || :


%package onedrive-signon
Summary:    Provides signon credentials refreshing with OneDrive
BuildRequires:  qt5-qttools-linguist
Requires: %{name} = %{version}-%{release}

%description onedrive-signon
%{summary}.

%files onedrive-signon
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/onedrive-signon-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libonedrive-signon-client.so
%config %{_sysconfdir}/buteo/profiles/client/onedrive-signon.xml
%config %{_sysconfdir}/buteo/profiles/sync/onedrive.Signon.xml

%pre onedrive-signon
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-signon.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.Signon.xml || :
done

%post onedrive-signon
systemctl-user try-restart msyncd.service || :


%package vk-posts
Summary:    Provides post synchronisation with VK
BuildRequires:  pkgconfig(Qt5Contacts)
BuildRequires:  pkgconfig(qtcontacts-sqlite-qt5-extensions)
BuildRequires:  qt5-qttools-linguist
Requires: %{name} = %{version}-%{release}

%description vk-posts
%{summary}.

%files vk-posts
#%{_datadir}/translations/lipstick-jolla-home-vk_eng_en.qm
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/vk-posts-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libvk-posts-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-posts.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Posts.xml

%pre vk-posts
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-posts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Posts.xml || :
done

%post vk-posts
systemctl-user restart msyncd.service || :

%package dropbox-images
Summary:    Provides image synchronisation with Dropbox
Requires: %{name} = %{version}-%{release}

%description dropbox-images
%{summary}.

%files dropbox-images
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/dropbox-images-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libdropbox-images-client.so
%config %{_sysconfdir}/buteo/profiles/client/dropbox-images.xml
%config %{_sysconfdir}/buteo/profiles/sync/dropbox.Images.xml

%pre dropbox-images
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/dropbox-images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/dropbox.Images.xml || :
done

%post dropbox-images
systemctl-user try-restart msyncd.service || :

%package onedrive-images
Summary:    Provides image synchronisation with OneDrive
Requires: %{name} = %{version}-%{release}

%description onedrive-images
%{summary}.

%files onedrive-images
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/onedrive-images-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libonedrive-images-client.so
%config %{_sysconfdir}/buteo/profiles/client/onedrive-images.xml
%config %{_sysconfdir}/buteo/profiles/sync/onedrive.Images.xml

%pre onedrive-images
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.Images.xml || :
done

%post onedrive-images
systemctl-user try-restart msyncd.service || :



%package onedrive-backup
Summary:    Provides backup-blob synchronization for OneDrive
BuildRequires:  qt5-qttools-linguist
Requires: %{name} = %{version}-%{release}

%description onedrive-backup
%{summary}.

%files onedrive-backup
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/onedrive-backup-client
%{_libdir}/buteo-plugins-qt5/oopp/onedrive-backupquery-client
%{_libdir}/buteo-plugins-qt5/oopp/onedrive-backuprestore-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libonedrive-backup-client.so
#%%{_libdir}/buteo-plugins-qt5/libonedrive-backupquery-client.so
#%%{_libdir}/buteo-plugins-qt5/libonedrive-backuprestore-client.so
%config %{_sysconfdir}/buteo/profiles/client/onedrive-backup.xml
%config %{_sysconfdir}/buteo/profiles/client/onedrive-backupquery.xml
%config %{_sysconfdir}/buteo/profiles/client/onedrive-backuprestore.xml
%config %{_sysconfdir}/buteo/profiles/sync/onedrive.Backup.xml
%config %{_sysconfdir}/buteo/profiles/sync/onedrive.BackupQuery.xml
%config %{_sysconfdir}/buteo/profiles/sync/onedrive.BackupRestore.xml

%pre onedrive-backup
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-backup.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-backupquery.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-backuprestore.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.Backup.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.BackupQuery.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.BackupRestore.xml || :
done

%post onedrive-backup
systemctl-user try-restart msyncd.service || :



%package dropbox-backup
Summary:    Provides backup-blob synchronization for Dropbox
BuildRequires:  qt5-qttools-linguist
Requires: %{name} = %{version}-%{release}

%description dropbox-backup
%{summary}.

%files dropbox-backup
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/dropbox-backup-client
%{_libdir}/buteo-plugins-qt5/oopp/dropbox-backupquery-client
%{_libdir}/buteo-plugins-qt5/oopp/dropbox-backuprestore-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libdropbox-backup-client.so
#%%{_libdir}/buteo-plugins-qt5/libdropbox-backupquery-client.so
#%%{_libdir}/buteo-plugins-qt5/libdropbox-backuprestore-client.so
%config %{_sysconfdir}/buteo/profiles/client/dropbox-backup.xml
%config %{_sysconfdir}/buteo/profiles/client/dropbox-backupquery.xml
%config %{_sysconfdir}/buteo/profiles/client/dropbox-backuprestore.xml
%config %{_sysconfdir}/buteo/profiles/sync/dropbox.Backup.xml
%config %{_sysconfdir}/buteo/profiles/sync/dropbox.BackupQuery.xml
%config %{_sysconfdir}/buteo/profiles/sync/dropbox.BackupRestore.xml

%pre dropbox-backup
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/dropbox-backup.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/client/dropbox-backupquery.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/client/dropbox-backuprestore.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/dropbox.Backup.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/dropbox.BackupQuery.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/dropbox.BackupRestore.xml || :
done

%post dropbox-backup
systemctl-user try-restart msyncd.service || :



%package vk-notifications
Summary:    Provides notification synchronisation with VK
BuildRequires:  nemo-qml-plugin-notifications-qt5-devel
BuildRequires:  qt5-qttools-linguist
Requires: %{name} = %{version}-%{release}

%description vk-notifications
%{summary}.

%files vk-notifications
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/vk-notifications-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libvk-notifications-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-notifications.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Notifications.xml

%pre vk-notifications
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-notifications.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Notifications.xml || :
done

%post vk-notifications
systemctl-user restart msyncd.service || :


%package vk-calendars
Summary:    Provides calendar synchronisation with VK
BuildRequires:  pkgconfig(libmkcal-qt5)
BuildRequires:  pkgconfig(libkcalcoren-qt5)
Requires: %{name} = %{version}-%{release}

%description vk-calendars
%{summary}.

%files vk-calendars
#out-of-proces-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/vk-calendars-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libvk-calendars-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-calendars.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Calendars.xml

%pre vk-calendars
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-calendars.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Calendars.xml || :
done

%post vk-calendars
systemctl-user restart msyncd.service || :


%package vk-contacts
Summary:    Provides contact synchronisation with VK
BuildRequires:  pkgconfig(Qt5Gui)
BuildRequires:  pkgconfig(Qt5Contacts)
BuildRequires:  pkgconfig(qtcontacts-sqlite-qt5-extensions)
Requires: %{name} = %{version}-%{release}

%description vk-contacts
%{summary}.

%files vk-contacts
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/vk-contacts-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libvk-contacts-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-contacts.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Contacts.xml

%pre vk-contacts
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-contacts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Contacts.xml || :
done

%post vk-contacts
systemctl-user restart msyncd.service || :


%package vk-images
Summary:    Provides image synchronisation with VK
Requires: %{name} = %{version}-%{release}

%description vk-images
%{summary}.

%files vk-images
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/vk-images-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libvk-images-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-images.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Images.xml

%pre vk-images
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Images.xml || :
done

%post vk-images
systemctl-user restart msyncd.service || :



%package knowncontacts
Summary: Store locally created contacts

%description knowncontacts
Buteo sync plugin that stores locally created contacts, such as email
recipients.

%post knowncontacts
systemctl-user try-restart msyncd.service || :

%files knowncontacts
%defattr(-,root,root,-)
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/knowncontacts-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/knowncontacts-client.so
%{_sysconfdir}/buteo/profiles/client/knowncontacts.xml
%{_sysconfdir}/buteo/profiles/sync/knowncontacts.Contacts.xml



%package ts-devel
Summary:    Translation source for sociald

%description ts-devel
%{summary}.

%files ts-devel
%defattr(-,root,root,-)
%{_datadir}/translations/source/lipstick-jolla-home-twitter-notif.ts

%package tests
Summary:    Automatable tests for sociald
BuildRequires:  pkgconfig(Qt5Test)
Requires:   qt5-qtdeclarative-devel-tools
Requires:   qt5-qtdeclarative-import-qttest

%description tests
%{summary}.

%files tests
%defattr(-,root,root,-)
/opt/tests/sociald/*


%prep
%setup -q -n %{name}-%{version}

%build
%qmake5 "DEFINES+=OUT_OF_PROCESS_PLUGIN"
make %{_smp_mflags}

%pre
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/sociald.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.facebook.Calendars.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.facebook.Contacts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.facebook.Images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.facebook.Notifications.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.twitter.Notifications.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.twitter.Posts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.google.Calendars.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/sociald.google.Contacts.xml || :
done

%install
rm -rf %{buildroot}
%qmake5_install

%post
systemctl-user try-restart msyncd.service || :

