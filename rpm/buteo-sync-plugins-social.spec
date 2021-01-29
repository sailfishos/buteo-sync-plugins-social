Name:       buteo-sync-plugins-social
Summary:    Sync plugins for social services
Version:    0.4.0
Release:    1
License:    LGPLv2
URL:        https://git.sailfishos.org/mer-core/buteo-sync-plugins-social
Source0:    %{name}-%{version}.tar.bz2
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5DBus)
BuildRequires:  pkgconfig(Qt5Sql)
BuildRequires:  pkgconfig(Qt5Network)
BuildRequires:  pkgconfig(Qt5Gui)
BuildRequires:  pkgconfig(Qt5Contacts)
BuildRequires:  qt5-qttools-linguist
BuildRequires:  pkgconfig(mlite5)
BuildRequires:  pkgconfig(buteosyncfw5) >= 0.6.36
BuildRequires:  pkgconfig(libsignon-qt5)
BuildRequires:  pkgconfig(accounts-qt5) >= 1.13
BuildRequires:  pkgconfig(socialcache) >= 0.0.48
BuildRequires:  pkgconfig(libsailfishkeyprovider)
BuildRequires:  pkgconfig(contactcache-qt5)
BuildRequires:  pkgconfig(qtcontacts-sqlite-qt5-extensions) >= 0.3.0
BuildRequires:  pkgconfig(libmkcal-qt5) >= 0.5.9
BuildRequires:  pkgconfig(KF5CalendarCore)
BuildRequires:  nemo-qml-plugin-notifications-qt5-devel
Requires: buteo-syncfw-qt5-msyncd
Requires: systemd
Requires(pre):  sailfish-setup
Requires(post): systemd
Obsoletes: sociald < 0.4.0
Provides: sociald

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
%{_libdir}/libsyncpluginscommon.so.*
%exclude %{_libdir}/libsyncpluginscommon.so
%license COPYING

%package facebook
Summary:    Provides synchronisation with Facebook
Requires: %{name} = %{version}-%{release}
Obsoletes: sociald-facebook-calendars < 0.4.0
Obsoletes: sociald-facebook-images < 0.4.0
Obsoletes: sociald-facebook-signon < 0.4.0
Provides: sociald-facebook-calendars
Provides: sociald-facebook-images
Provides: sociald-facebook-signon

%description facebook
%{summary}.

%files facebook
# calendar:
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/facebook-calendars-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libfacebook-calendars-client.so
%config %{_sysconfdir}/buteo/profiles/client/facebook-calendars.xml
%config %{_sysconfdir}/buteo/profiles/sync/facebook.Calendars.xml
# images:
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/facebook-images-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libfacebook-images-client.so
%config %{_sysconfdir}/buteo/profiles/client/facebook-images.xml
%config %{_sysconfdir}/buteo/profiles/sync/facebook.Images.xml
# signon
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/facebook-signon-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libfacebook-signon-client.so
%config %{_sysconfdir}/buteo/profiles/client/facebook-signon.xml
%config %{_sysconfdir}/buteo/profiles/sync/facebook.Signon.xml


%pre facebook
# calendar
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/facebook-calendars.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/facebook.Calendars.xml || :
done
#images
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/facebook-images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/facebook.Images.xml || :
done
#signon
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/facebook-signon.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/facebook.Signon.xml || :
done

%post facebook
systemctl-user try-restart msyncd.service || :


%package google
Summary:    Provides synchronisation with Google
Requires: %{name} = %{version}-%{release}
Obsoletes: sociald-google-calendars < 0.4.0
Obsoletes: sociald-google-contacts < 0.4.0
Obsoletes: sociald-google-signon < 0.4.0
Provides: sociald-google-calendars
Provides: sociald-google-contacts
Provides: sociald-google-signon


%description google
%{summary}.

%files google
# calendar
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/google-calendars-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libgoogle-calendars-client.so
%config %{_sysconfdir}/buteo/profiles/client/google-calendars.xml
%config %{_sysconfdir}/buteo/profiles/sync/google.Calendars.xml
# contacts
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/google-contacts-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libgoogle-contacts-client.so
%config %{_sysconfdir}/buteo/profiles/client/google-contacts.xml
%config %{_sysconfdir}/buteo/profiles/sync/google.Contacts.xml
# signon
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/google-signon-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libgoogle-signon-client.so
%config %{_sysconfdir}/buteo/profiles/client/google-signon.xml
%config %{_sysconfdir}/buteo/profiles/sync/google.Signon.xml


%pre google
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
# calendar
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/google-calendars.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/google.Calendars.xml || :
done
# contacts
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/google-contacts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/google.Contacts.xml || :
done
# signon
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/google-signon.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/google.Signon.xml || :
done


%package twitter
Summary:    Provides synchronisation with Twitter
Requires: %{name} = %{version}-%{release}
Obsoletes: sociald-twitter-notifications < 0.4.0
Obsoletes: sociald-twitter-posts < 0.4.0
Provides: sociald-twitter-notifications
Provides: sociald-twitter-posts

%description twitter
%{summary}.

%files twitter
# notifications
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/twitter-notifications-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libtwitter-notifications-client.so
%config %{_sysconfdir}/buteo/profiles/client/twitter-notifications.xml
%config %{_sysconfdir}/buteo/profiles/sync/twitter.Notifications.xml
%{_datadir}/translations/lipstick-jolla-home-twitter-notif_eng_en.qm
# posts
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/twitter-posts-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libtwitter-posts-client.so
%config %{_sysconfdir}/buteo/profiles/client/twitter-posts.xml
%config %{_sysconfdir}/buteo/profiles/sync/twitter.Posts.xml

%pre twitter
# notifications
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/twitter-notifications.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/twitter.Notifications.xml || :
done
# posts
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/twitter-posts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/twitter.Posts.xml || :
done

%post twitter
systemctl-user try-restart msyncd.service || :

%package onedrive
Summary:    Provides synchronisation with OneDrive
Requires: %{name} = %{version}-%{release}
Obsoletes: sociald-onedrive-signon < 0.4.0
Obsoletes: sociald-onedrive-images < 0.4.0
Obsoletes: sociald-onedrive-backup < 0.4.0
Provides: sociald-onedrive-signon
Provides: sociald-onedrive-images
Provides: sociald-onedrive-backup

%description onedrive
%{summary}.

%files onedrive
# signon
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/onedrive-signon-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libonedrive-signon-client.so
%config %{_sysconfdir}/buteo/profiles/client/onedrive-signon.xml
%config %{_sysconfdir}/buteo/profiles/sync/onedrive.Signon.xml
# images
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/onedrive-images-client
# backup
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libonedrive-images-client.so
%config %{_sysconfdir}/buteo/profiles/client/onedrive-images.xml
%config %{_sysconfdir}/buteo/profiles/sync/onedrive.Images.xml
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

%pre onedrive
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
# signon
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-signon.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.Signon.xml || :
done
# images
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.Images.xml || :
done
# backup
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-backup.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-backupquery.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/client/onedrive-backuprestore.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.Backup.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.BackupQuery.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/onedrive.BackupRestore.xml || :
done

%post onedrive
systemctl-user try-restart msyncd.service || :


%package vk
Summary:    Provides synchronisation with VK
Requires: %{name} = %{version}-%{release}
Obsoletes: sociald-vk-posts < 0.4.0
Obsoletes: sociald-vk-notifications < 0.4.0
Obsoletes: sociald-vk-calendars < 0.4.0
Obsoletes: sociald-vk-contacts < 0.4.0
Obsoletes: sociald-vk-images < 0.4.0
Provides: sociald-vk-posts
Provides: sociald-vk-notifications
Provides: sociald-vk-calendars
Provides: sociald-vk-contacts
Provides: sociald-vk-images

%description vk
%{summary}.

%files vk
# posts
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/vk-posts-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libvk-posts-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-posts.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Posts.xml
# notifications
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/vk-notifications-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libvk-notifications-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-notifications.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Notifications.xml
# calendars
#out-of-proces-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/vk-calendars-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libvk-calendars-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-calendars.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Calendars.xml
# contacts
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/vk-contacts-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libvk-contacts-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-contacts.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Contacts.xml
# images
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/vk-images-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libvk-images-client.so
%config %{_sysconfdir}/buteo/profiles/client/vk-images.xml
%config %{_sysconfdir}/buteo/profiles/sync/vk.Images.xml


%pre vk
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
# posts
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-posts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Posts.xml || :
done
# notifications
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-notifications.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Notifications.xml || :
done
# calendars
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-calendars.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Calendars.xml || :
done
# contacts
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-contacts.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Contacts.xml || :
done
# images
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/vk-images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/vk.Images.xml || :
done


%post vk
systemctl-user restart msyncd.service || :


%package dropbox
Summary:    Provides synchronisation with Dropbox
Requires: %{name} = %{version}-%{release}
Obsoletes: sociald-dropbox-images < 0.4.0
Obsoletes: sociald-dropbox-backup < 0.4.0
Provides: sociald-dropbox-images
Provides: sociald-dropbox-backup

%description dropbox
%{summary}.

%files dropbox
# images
#out-of-process-plugin form:
%{_libdir}/buteo-plugins-qt5/oopp/dropbox-images-client
#in-process-plugin form:
#%%{_libdir}/buteo-plugins-qt5/libdropbox-images-client.so
%config %{_sysconfdir}/buteo/profiles/client/dropbox-images.xml
%config %{_sysconfdir}/buteo/profiles/sync/dropbox.Images.xml
# backup
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

%pre dropbox
USERS=$(getent group users | cut -d ":" -f 4 | tr "," "\n")
# images
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/dropbox-images.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/dropbox.Images.xml || :
done
# backup
for user in $USERS; do
    USERHOME=$(getent passwd ${user} | cut -d ":" -f 6)
    rm -f ${USERHOME}/.cache/msyncd/sync/client/dropbox-backup.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/client/dropbox-backupquery.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/client/dropbox-backuprestore.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/dropbox.Backup.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/dropbox.BackupQuery.xml || :
    rm -f ${USERHOME}/.cache/msyncd/sync/dropbox.BackupRestore.xml || :
done

%post dropbox
systemctl-user try-restart msyncd.service || :


%package knowncontacts
Summary: Store locally created contacts
Requires: %{name} = %{version}-%{release}
Obsoletes: sociald-knowncontacts < 0.4.0
Provides: sociald-knowncontacts

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


%prep
%setup -q -n %{name}-%{version}

%build
%qmake5 "DEFINES+=OUT_OF_PROCESS_PLUGIN" \
    "CONFIG+=dropbox" \
    "CONFIG+=facebook" \
    "CONFIG+=google" \
    "CONFIG+=onedrive" \
    "CONFIG+=twitter" \
    "CONFIG+=vk" \
    "CONFIG+=knowncontacts" \
    "CONFIG+=calendar"
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
/sbin/ldconfig || :
systemctl-user try-restart msyncd.service || :

%postun
/sbin/ldconfig || :
