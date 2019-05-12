/****************************************************************************
**
** Copyright (C) 2013 Jolla Ltd, author: <valerio.valerio@jollamobile.com>
** Contact: http://www.qt.io/licensing/
**
** This file is part of the Qt Messaging Framework.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qmailmessageserverplugin.h"
#include <qmailpluginmanager.h>
#include <QDebug>

#define PLUGIN_KEY "messageserverplugins"

typedef QMap<QString, QMailMessageServerPlugin*> PluginMap;

/*! \internal
    Load all the plugins into a map for quicker reference
*/
static PluginMap initMap(QMailPluginManager& manager)
{
    PluginMap map;

    foreach (const QString &item, manager.list()) {
        QObject *instance = manager.instance(item);
        if (QMailMessageServerPlugin* iface = qobject_cast<QMailMessageServerPlugin*>(instance))
                map.insert(iface->key(), iface);
    }
    return map;
}
/*! \internal
    Return a reference to a map containing all loaded plugin objects
*/
static PluginMap& pluginMap()
{
    static QMailPluginManager pluginManager(QStringLiteral(PLUGIN_KEY));
    static PluginMap map(initMap(pluginManager));

    return map;
}

/*! \internal
    Return the plugin object matching the specified ID
*/
static QMailMessageServerPlugin* mapping(const QString& key)
{
    PluginMap::ConstIterator it;
    if ((it = pluginMap().find(key)) != pluginMap().end())
        return it.value();

    qWarning() << "Failed attempt to map plugin: " << key;
    return Q_NULLPTR;
}

/*!
    \class QMailMessageServerPlugin
    \ingroup libmessageserver

    \brief The QMailMessageServerPlugin class defines the interface to plugins that provide additional services
    for the messageserver daemon.

    The QMailMessageServerPlugin class defines the interface to messageserver plugins.  Plugins will
    be loaded and executed in the messageserver main loop, these plugins should only be used for services
    that need to be informed about operations initiated by all clients connected to messageserver.

    \sa QMailMessageServerPluginFactory
*/

/*!
    \fn QString QMailMessageServerPlugin::key() const

    Returns a string identifying the plugin.
*/

/*!
    \fn QString QMailMessageServerPlugin::exec()

    The starting point for the plugin.
    Plugin implementations should use this function as execution
    entry point for the service(s) provided by the plugin.
*/

/*!
    \fn QMailMessageServerPlugin* QMailMessageServerPlugin::createService()

    Creates an instance of the service provided by the plugin.
*/


QMailMessageServerPlugin::QMailMessageServerPlugin(QObject *parent)
    : QObject(parent)
{
}

QMailMessageServerPlugin::~QMailMessageServerPlugin()
{
}

/*!
    \class QMailMessageServerPluginFactory
    \ingroup libmessageserver

    \brief The QMailMessageServerPluginFactory class creates objects implementing the QMailMessageServerPlugin interface.

    The QMailMessageServerPluginFactory class creates objects that provide plugins services to the
    messageserver daemon. The factory allows implementations to be loaded from plugin libraries,
    and to be retrieved and instantiated by name.

    To create a new service that can be created via the QMailMessageServerPluginFactory, derive from the
    QMailMessageServerPlugin base class.

    \sa QMailMessageServerPlugin
*/

/*!
    Returns a list of the keys of the installed plugins.
 */
QStringList QMailMessageServerPluginFactory::keys()
{
    QStringList in;

    foreach (PluginMap::mapped_type plugin, pluginMap())
        in << plugin->key();

    return in;
}

/*!
    Creates a plugin object of the class identified by \a key.
*/
QMailMessageServerPlugin* QMailMessageServerPluginFactory::createService(const QString& key)
{
    if (QMailMessageServerPlugin* plugin = mapping(key))
        return plugin->createService();

    return Q_NULLPTR;
}
