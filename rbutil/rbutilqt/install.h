/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 *   Copyright (C) 2007 by Dominik Riebeling
 *   $Id$
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#ifndef INSTALL_H
#define INSTALL_H

#include <QtGui>

#include "ui_installfrm.h"
#include "zipinstaller.h"
#include "progressloggergui.h"

class Install : public QDialog
{
    Q_OBJECT
    public:
        Install(QWidget *parent);
        void setVersionStrings(QMap<QString, QString>&);

    public slots:
        void accept(void);

    private:
        Ui::InstallFrm ui;
        ProgressLoggerGui* logger;
        QHttp *download;
        QFile *target;
        QString file;
        ZipInstaller* installer;
        QMap<QString, QString> version;
        QString m_backupName;
        void resizeEvent(QResizeEvent*);

        void changeBackupPath(QString);
        void updateBackupLocation(void);

    private slots:
        void setDetailsCurrent(bool);
        void setDetailsStable(bool);
        void setDetailsArchived(bool);
        void done(bool);
        void changeBackupPath(void);
        void backupCheckboxChanged(int state);

};


#endif
