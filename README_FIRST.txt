Inverted TaskFlow Server - read me first
=========================================

This package is for the IT administrator. Everything is explained in
IT_Admin_Guide_v*.docx (the same text is in IT_Admin_Guide_v*.md).

Production server address used by the desktop apps:  https://task.inverted.in:8443
(the DNS record task.inverted.in must point to the server; the HTTPS certificate must cover that name)

Recommended installation (about 10 minutes): an Ubuntu 22.04 / 24.04 LTS server or VM
  1. Copy this zip to the server and unzip it:   unzip InvertedTaskFlow_Server_v*.zip
  2. cd InvertedTaskFlow_Server_v*/deploy/linux
  3. sudo bash install_server.sh --host task.inverted.in
  4. Note the two values it prints - the server address and the certificate fingerprint -
     and give them to the person who builds the desktop installer (build_installer.bat).
  5. sudo cat /var/lib/taskflow/initial_admin_password.txt  -> sign in with the desktop app,
     choose a new password, delete the file.

Day-to-day:   taskflowctl status | logs | backup | restore | check | reset-password | update

Docker instead of the installer: see DOCKER.md (one or the other - never both on one database).
Windows Server instead of Linux: see the "windows" folder and the guide (chapter 12).
