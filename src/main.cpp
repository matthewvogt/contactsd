/***************************************************************************
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (people-users@projects.maemo.org)
**
** This file is part of contactsd.
**
** If you have questions regarding the use of this file, please contact
** Nokia at people-users@projects.maemo.org.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation
** and appearing in the file LICENSE.LGPL included in the packaging
** of this file.
**
****************************************************************************/

#include <QDebug>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QTimer>

#include "contactsd.h"
#include "logger.h"

static void usage()
{
    qDebug() << "Usage: contactsd [OPTION]...\n";
    qDebug() << "  --plugins PLUGINS    Comma separated list of plugins to load\n";
    qDebug() << "  --log-console        Enable Console Logging \n";
    qDebug() << "  --version            Output version information and exit";
    qDebug() << "  --help               Display this help and exit";
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QStringList plugins;
    const QStringList args = app.arguments();
    Logger *logger = Logger::installLogger(CONTACTSD_LOG_DIR "/contactsd.log", 50, 3);
    QString arg;
    int i = 1; // ignore argv[0]

    logger->setParent(&app);
    logger->setConsoleLoggingEnabled(!qgetenv("CONTACTSD_DEBUG").isEmpty());
    while (i < args.count()) {
        arg = args.at(i);
        if (arg == "--plugins") {
            if (++i == args.count()) {
                usage();
                return -1;
            }

            QString value = args.at(i);
            value.replace(" ", ",");
            plugins << value.split(",", QString::SkipEmptyParts);
        } else if (arg == "--version") {
            qDebug() << "contactsd version" << VERSION;
            return 0;
        } else if (arg == "--help") {
            usage();
            return 0;
        } else if (arg == "--log-console") {
            logger->setConsoleLoggingEnabled(true);
        } else{
            qWarning() << "Invalid argument" << arg;
            usage();
            return -1;
        }
        ++i;
    }

    qDebug() << "contactsd version" << VERSION << "started";

    Contactsd *daemon = new Contactsd(&app);
    daemon->loadPlugins(plugins);

    return app.exec();
}
