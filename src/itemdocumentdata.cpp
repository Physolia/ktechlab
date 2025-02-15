/***************************************************************************
 *   Copyright (C) 2005 by David Saxton                                    *
 *   david@bluehaze.org                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#include "itemdocumentdata.h"
#include "connector.h"
#include "ecsubcircuit.h"
#include "electronicconnector.h"
#include "flowcodedocument.h"
#include "flowconnector.h"
#include "flowcontainer.h"
#include "itemlibrary.h"
#include "junctionflownode.h"
#include "junctionnode.h"
#include "picitem.h"
#include "pinmapping.h"

#include <KIO/FileCopyJob>
#include <KJobWidgets>
#include <KLocalizedString>
#include <KMessageBox>

#include <QBitArray>
#include <QFile>
#include <QScopedPointer>
#include <QTemporaryFile>

#include <ktechlab_debug.h>

// Converts the QBitArray into a string (e.g. "F289A9E") that can be stored in an xml file
static QString toAsciiHex(QBitArray _data)
{
    QBitArray data = _data;
    // 	data = qCompress(data);

    // Pad out the data to a nice size
    if ((data.size() % 4) != 0) {
        data.detach();
        data.resize(data.size() + 4 - (data.size() % 4));
    }

    QString text;
    for (int i = 0; i < data.size() / 4; ++i) {
        unsigned val = 0;
        for (unsigned j = 0; j < 4; ++j)
            val += (data[4 * i + j] ? 1 : 0) << j;

        text += QString::number(val, 16);
    }
    return text;
}

// Converts a string (e.g. "F289A9E") into a QBitArray, the opposite of the above function
static QBitArray toQBitArray(QString text)
{
    unsigned size = text.length();
    QBitArray data(size * 4);

    for (unsigned i = 0; i < size; ++i) {
        unsigned val = QString(text[i]).toInt(nullptr, 16);
        for (unsigned j = 0; j < 4; ++j)
            data[4 * i + j] = val & (1 << j);
    }

    // 	data = qUncompress(data);

    return data;
}

// BEGIN class ItemDocumentData
ItemDocumentData::ItemDocumentData(uint documentType)
{
    reset();
    m_documentType = documentType;
}

ItemDocumentData::~ItemDocumentData()
{
}

void ItemDocumentData::reset()
{
    m_itemDataMap.clear();
    m_connectorDataMap.clear();
    m_nodeDataMap.clear();
    m_microData.reset();
    m_documentType = Document::dt_none;
}

bool ItemDocumentData::loadData(const QUrl &url)
{
    QScopedPointer<QFile> file;
    if (!url.isLocalFile()) {
        QScopedPointer<QTemporaryFile> downloadedFile(new QTemporaryFile());
        downloadedFile->open();
        KIO::FileCopyJob *job = KIO::file_copy(url, QUrl::fromLocalFile(downloadedFile->fileName()));
        KJobWidgets::setWindow(job, nullptr);
        if (!job->exec()) {
            KMessageBox::error(nullptr, job->errorString());
            return false;
        }
        file.reset(downloadedFile.take());
    } else {
        QScopedPointer<QFile> localFile(new QFile(url.toLocalFile()));
        if (!localFile->open(QIODevice::ReadOnly)) {
            KMessageBox::error(nullptr, i18n("Could not open %1 for reading", localFile->fileName()));
            return false;
        }
        file.reset(localFile.take());
    }

    QString xml;
    QTextStream textStream(file.data());
    while (!textStream.atEnd() /* eof() */)
        xml += textStream.readLine() + '\n';

    return fromXML(xml);
}

bool ItemDocumentData::fromXML(const QString &xml)
{
    reset();

    QDomDocument doc("KTechlab");
    QString errorMessage;
    if (!doc.setContent(xml, &errorMessage)) {
        KMessageBox::error(nullptr, i18n("Could not parse XML:\n%1", errorMessage));
        return false;
    }

    QDomElement root = doc.documentElement();

    QDomNode node = root.firstChild();
    while (!node.isNull()) {
        QDomElement element = node.toElement();
        if (!element.isNull()) {
            const QString tagName = element.tagName();

            if (tagName == "item")
                elementToItemData(element);

            else if (tagName == "node")
                elementToNodeData(element);

            else if (tagName == "connector")
                elementToConnectorData(element);

            else if (tagName == "pic-settings" || tagName == "micro")
                elementToMicroData(element);

            else if (tagName == "code")
                ; // do nothing - we no longer use this tag

            else
                qCWarning(KTL_LOG) << "Unrecognised element tag name: " << tagName;
        }

        node = node.nextSibling();
    }

    return true;
}

bool ItemDocumentData::saveData(const QUrl &url)
{
    if (url.isLocalFile()) {
        QFile file(url.toLocalFile());
        if (!file.open(QIODevice::WriteOnly)) {
            KMessageBox::error(nullptr, i18n("Could not open '%1' for writing. Check that you have write permissions", file.fileName()), i18n("Saving File"));
            return false;
        }

        QTextStream stream(&file);
        stream << toXML();
        file.close();
    } else {
        QTemporaryFile file;
        if (!file.open()) {
            KMessageBox::error(nullptr, file.errorString());
            return false;
        }
        QTextStream str(&file);
        str << toXML();
        file.close();

        KIO::FileCopyJob *job = KIO::file_copy(QUrl::fromLocalFile(file.fileName()), url);
        KJobWidgets::setWindow(job, nullptr);
        if (!job->exec()) {
            KMessageBox::error(nullptr, job->errorString());
            return false;
        }
    }

    return true;
}

QString ItemDocumentData::toXML()
{
    QDomDocument doc("KTechlab");
    // TODO Add revision information to save file

    QDomElement root = doc.createElement("document");
    root.setAttribute("type", documentTypeString());
    doc.appendChild(root);

    {
        const ItemDataMap::iterator end = m_itemDataMap.end();
        for (ItemDataMap::iterator it = m_itemDataMap.begin(); it != end; ++it) {
            QDomElement node = itemDataToElement(doc, it.value());
            node.setAttribute("id", it.key());
            root.appendChild(node);
        }
    }
    {
        const ConnectorDataMap::iterator end = m_connectorDataMap.end();
        for (ConnectorDataMap::iterator it = m_connectorDataMap.begin(); it != end; ++it) {
            QDomElement node = connectorDataToElement(doc, it.value());
            node.setAttribute("id", it.key());
            root.appendChild(node);
        }
    }
    {
        const NodeDataMap::iterator end = m_nodeDataMap.end();
        for (NodeDataMap::iterator it = m_nodeDataMap.begin(); it != end; ++it) {
            QDomElement node = nodeDataToElement(doc, it.value());
            node.setAttribute("id", it.key());
            root.appendChild(node);
        }
    }
    if (m_documentType == Document::dt_flowcode) {
        QDomElement node = microDataToElement(doc);
        root.appendChild(node);
    }

    return doc.toString();
}

// BEGIN functions for generating / reading QDomElements
QDomElement ItemDocumentData::microDataToElement(QDomDocument &doc)
{
    QDomElement node = doc.createElement("micro");
    node.setAttribute("id", m_microData.id);

    {
        const PinMappingMap::iterator end = m_microData.pinMappings.end();
        for (PinMappingMap::iterator it = m_microData.pinMappings.begin(); it != end; ++it) {
            QDomElement pinMapNode = doc.createElement("pinmap");

            QString type;
            switch (it.value().type()) {
            case PinMapping::SevenSegment:
                type = "sevensegment";
                break;

            case PinMapping::Keypad_4x3:
                type = "keypad_4x3";
                break;

            case PinMapping::Keypad_4x4:
                type = "keypad_4x4";
                break;

            case PinMapping::Invalid:
                break;
            }

            pinMapNode.setAttribute("id", it.key());
            pinMapNode.setAttribute("type", type);
            pinMapNode.setAttribute("map", it.value().pins().join(" "));

            node.appendChild(pinMapNode);
        }
    }

    {
        const PinDataMap::iterator end = m_microData.pinMap.end();
        for (PinDataMap::iterator it = m_microData.pinMap.begin(); it != end; ++it) {
            QDomElement pinNode = doc.createElement("pin");

            pinNode.setAttribute("id", it.key());
            pinNode.setAttribute("type", (it.value().type == PinSettings::pt_input) ? "input" : "output");
            pinNode.setAttribute("state", (it.value().state == PinSettings::ps_off) ? "off" : "on");

            node.appendChild(pinNode);
        }
    }

    {
        const QStringMap::iterator end = m_microData.variableMap.end();
        for (QStringMap::iterator it = m_microData.variableMap.begin(); it != end; ++it) {
            QDomElement variableNode = doc.createElement("variable");

            variableNode.setAttribute("name", it.key());
            variableNode.setAttribute("value", it.value());

            node.appendChild(variableNode);
        }
    }

    return node;
}

void ItemDocumentData::elementToMicroData(QDomElement element)
{
    QString id = element.attribute("id", QString());

    if (id.isNull())
        id = element.attribute("pic", QString());

    if (id.isNull()) {
        qCCritical(KTL_LOG) << "Could not find id in element";
        return;
    }

    m_microData.reset();
    m_microData.id = id;

    QDomNode node = element.firstChild();
    while (!node.isNull()) {
        QDomElement childElement = node.toElement();
        if (!childElement.isNull()) {
            const QString tagName = childElement.tagName();

            if (tagName == "pinmap") {
                QString id = childElement.attribute("id", QString());
                QString typeString = childElement.attribute("type", QString());

                if (!id.isEmpty() && !typeString.isEmpty()) {
                    PinMapping::Type type = PinMapping::Invalid;

                    if (typeString == "sevensegment")
                        type = PinMapping::SevenSegment;

                    else if (typeString == "keypad_4x3")
                        type = PinMapping::Keypad_4x3;

                    else if (typeString == "keypad_4x4")
                        type = PinMapping::Keypad_4x4;

                    PinMapping pinMapping(type);
                    // pinMapping.setPins( QStringList::split( " ", childElement.attribute( "map", 0 ) ) ); // 2018.12.01
                    pinMapping.setPins(childElement.attribute("map", nullptr).split(" ", Qt::SkipEmptyParts));

                    m_microData.pinMappings[id] = pinMapping;
                }
            }

            else if (tagName == "pin") {
                QString pinID = childElement.attribute("id", QString());
                if (!pinID.isEmpty()) {
                    m_microData.pinMap[pinID].type = (childElement.attribute("type", "input") == "input") ? PinSettings::pt_input : PinSettings::pt_output;
                    m_microData.pinMap[pinID].state = (childElement.attribute("state", "off") == "off") ? PinSettings::ps_off : PinSettings::ps_on;
                }
            }

            else if (tagName == "variable") {
                QString variableId = childElement.attribute("name", QString());
                m_microData.variableMap[variableId] = childElement.attribute("value", QString());
            }

            else
                qCCritical(KTL_LOG) << "Unrecognised element tag name: " << tagName;
        }

        node = node.nextSibling();
    }
}

QDomElement ItemDocumentData::itemDataToElement(QDomDocument &doc, const ItemData &itemData)
{
    QDomElement node = doc.createElement("item");
    node.setAttribute("type", itemData.type);
    node.setAttribute("x", itemData.x);
    node.setAttribute("y", itemData.y);
    if (itemData.z != -1)
        node.setAttribute("z", itemData.z);
    if (itemData.setSize) {
        node.setAttribute("offset-x", itemData.size.x());
        node.setAttribute("offset-y", itemData.size.y());
        node.setAttribute("width", itemData.size.width());
        node.setAttribute("height", itemData.size.height());
    }

    // If the "orientation" is >= 0, then set by a FlowPart, so we don't need to worry about the angle / flip
    if (itemData.orientation >= 0) {
        node.setAttribute("orientation", itemData.orientation);
    } else {
        node.setAttribute("angle", itemData.angleDegrees);
        node.setAttribute("flip", itemData.flipped);
    }

    if (!itemData.parentId.isEmpty())
        node.setAttribute("parent", itemData.parentId);

    const QStringMap::const_iterator stringEnd = itemData.dataString.end();
    for (QStringMap::const_iterator it = itemData.dataString.begin(); it != stringEnd; ++it) {
        QDomElement e = doc.createElement("data");
        node.appendChild(e);
        e.setAttribute("id", it.key());
        e.setAttribute("type", "string");
        e.setAttribute("value", it.value());
    }

    const DoubleMap::const_iterator numberEnd = itemData.dataNumber.end();
    for (DoubleMap::const_iterator it = itemData.dataNumber.begin(); it != numberEnd; ++it) {
        QDomElement e = doc.createElement("data");
        node.appendChild(e);
        e.setAttribute("id", it.key());
        e.setAttribute("type", "number");
        e.setAttribute("value", QString::number(it.value()));
    }

    const QColorMap::const_iterator colorEnd = itemData.dataColor.end();
    for (QColorMap::const_iterator it = itemData.dataColor.begin(); it != colorEnd; ++it) {
        QDomElement e = doc.createElement("data");
        node.appendChild(e);
        e.setAttribute("id", it.key());
        e.setAttribute("type", "color");
        e.setAttribute("value", it.value().name());
    }

    const QBitArrayMap::const_iterator rawEnd = itemData.dataRaw.end();
    for (QBitArrayMap::const_iterator it = itemData.dataRaw.begin(); it != rawEnd; ++it) {
        QDomElement e = doc.createElement("data");
        node.appendChild(e);
        e.setAttribute("id", it.key());
        e.setAttribute("type", "raw");
        e.setAttribute("value", toAsciiHex(it.value()));
    }

    const BoolMap::const_iterator boolEnd = itemData.dataBool.end();
    for (BoolMap::const_iterator it = itemData.dataBool.begin(); it != boolEnd; ++it) {
        QDomElement e = doc.createElement("data");
        node.appendChild(e);
        e.setAttribute("id", it.key());
        e.setAttribute("type", "bool");
        e.setAttribute("value", QString::number(it.value()));
    }

    const BoolMap::const_iterator buttonEnd = itemData.buttonMap.end();
    for (BoolMap::const_iterator it = itemData.buttonMap.begin(); it != buttonEnd; ++it) {
        QDomElement e = doc.createElement("button");
        node.appendChild(e);
        e.setAttribute("id", it.key());
        e.setAttribute("state", QString::number(it.value()));
    }

    const IntMap::const_iterator sliderEnd = itemData.sliderMap.end();
    for (IntMap::const_iterator it = itemData.sliderMap.begin(); it != sliderEnd; ++it) {
        QDomElement e = doc.createElement("slider");
        node.appendChild(e);
        e.setAttribute("id", it.key());
        e.setAttribute("value", QString::number(it.value()));
    }

    return node;
}

void ItemDocumentData::elementToItemData(QDomElement element)
{
    QString id = element.attribute("id", QString());
    if (id.isNull()) {
        qCCritical(KTL_LOG) << "Could not find id in element";
        return;
    }

    ItemData itemData;
    itemData.type = element.attribute("type", QString());
    itemData.x = element.attribute("x", "120").toInt();
    itemData.y = element.attribute("y", "120").toInt();
    itemData.z = element.attribute("z", "-1").toInt();

    if (element.hasAttribute("width") && element.hasAttribute("height")) {
        itemData.setSize = true;
        itemData.size = QRect(element.attribute("offset-x", "0").toInt(), element.attribute("offset-y", "0").toInt(), element.attribute("width", "120").toInt(), element.attribute("height", "120").toInt());
    } else
        itemData.setSize = false;

    itemData.angleDegrees = element.attribute("angle", "0").toInt();
    itemData.flipped = element.attribute("flip", "0").toInt();
    itemData.orientation = element.attribute("orientation", "-1").toInt();
    itemData.parentId = element.attribute("parent", QString());

    m_itemDataMap[id] = itemData;

    QDomNode node = element.firstChild();
    while (!node.isNull()) {
        QDomElement childElement = node.toElement();
        if (!childElement.isNull()) {
            const QString tagName = childElement.tagName();

            if (tagName == "item") {
                // We're reading in a file saved in the older format, with
                // child items nestled, so we must specify that the new item
                // has the currently parsed item as its parent.
                elementToItemData(childElement);
                QString childId = childElement.attribute("id", QString());
                if (!childId.isNull())
                    m_itemDataMap[childId].parentId = id;
            }

            else if (tagName == "data") {
                QString dataId = childElement.attribute("id", QString());
                if (!dataId.isNull()) {
                    QString dataType = childElement.attribute("type", QString());
                    QString value = childElement.attribute("value", QString());

                    if (dataType == "string" || dataType == "multiline")
                        m_itemDataMap[id].dataString[dataId] = value;
                    else if (dataType == "number")
                        m_itemDataMap[id].dataNumber[dataId] = value.toDouble();
                    else if (dataType == "color")
                        m_itemDataMap[id].dataColor[dataId] = QColor(value);
                    else if (dataType == "raw")
                        m_itemDataMap[id].dataRaw[dataId] = toQBitArray(value);
                    else if (dataType == "bool")
                        m_itemDataMap[id].dataBool[dataId] = bool(value.toInt());
                    else
                        qCCritical(KTL_LOG) << "Unknown data type of \"" << dataType << "\" with id \"" << dataId << "\"";
                }
            }

            else if (tagName == "button") {
                QString buttonId = childElement.attribute("id", QString());
                if (!buttonId.isNull())
                    m_itemDataMap[id].buttonMap[buttonId] = childElement.attribute("state", "0").toInt();
            }

            else if (tagName == "slider") {
                QString sliderId = childElement.attribute("id", QString());
                if (!sliderId.isNull())
                    m_itemDataMap[id].sliderMap[sliderId] = childElement.attribute("value", "0").toInt();
            }

            else if (tagName == "child-node")
                ; // Tag name was used in 0.1 file save format

            else
                qCCritical(KTL_LOG) << "Unrecognised element tag name: " << tagName;
        }

        node = node.nextSibling();
    }
}

QDomElement ItemDocumentData::nodeDataToElement(QDomDocument &doc, const NodeData &nodeData)
{
    QDomElement node = doc.createElement("node");
    node.setAttribute("x", nodeData.x);
    node.setAttribute("y", nodeData.y);
    return node;
}

void ItemDocumentData::elementToNodeData(QDomElement element)
{
    QString id = element.attribute("id", QString());
    if (id.isNull()) {
        qCCritical(KTL_LOG) << "Could not find id in element";
        return;
    }

    NodeData nodeData;
    nodeData.x = element.attribute("x", "120").toInt();
    nodeData.y = element.attribute("y", "120").toInt();

    m_nodeDataMap[id] = nodeData;
}

QDomElement ItemDocumentData::connectorDataToElement(QDomDocument &doc, const ConnectorData &connectorData)
{
    QDomElement node = doc.createElement("connector");

    node.setAttribute("manual-route", connectorData.manualRoute);

    QString route;
    const QPointList::const_iterator end = connectorData.route.end();
    for (QPointList::const_iterator it = connectorData.route.begin(); it != end; ++it) {
        route.append(QString::number((*it).x()) + ",");
        route.append(QString::number((*it).y()) + ",");
    }
    node.setAttribute("route", route);

    if (connectorData.startNodeIsChild) {
        node.setAttribute("start-node-is-child", 1);
        node.setAttribute("start-node-cid", connectorData.startNodeCId);
        node.setAttribute("start-node-parent", connectorData.startNodeParent);
    } else {
        node.setAttribute("start-node-is-child", 0);
        node.setAttribute("start-node-id", connectorData.startNodeId);
    }

    if (connectorData.endNodeIsChild) {
        node.setAttribute("end-node-is-child", 1);
        node.setAttribute("end-node-cid", connectorData.endNodeCId);
        node.setAttribute("end-node-parent", connectorData.endNodeParent);
    } else {
        node.setAttribute("end-node-is-child", 0);
        node.setAttribute("end-node-id", connectorData.endNodeId);
    }

    return node;
}

void ItemDocumentData::elementToConnectorData(QDomElement element)
{
    QString id = element.attribute("id", QString());
    if (id.isNull()) {
        qCCritical(KTL_LOG) << "Could not find id in element";
        return;
    }

    ConnectorData connectorData;

    connectorData.manualRoute = (element.attribute("manual-route", "0") == "1");
    QString route = element.attribute("route", "");

    QStringList points = route.split(",", Qt::SkipEmptyParts); // QStringList::split( ",", route ); // 2018.12.01
    if (route.isEmpty()) {
        points = QStringList();
    }
    qCDebug(KTL_LOG) << "points=" << points;
    const QStringList::iterator end = points.end();
    for (QStringList::iterator it = points.begin(); it != end; ++it) {
        int x = (*it).toInt();
        it++;
        if (it != end) {
            int y = (*it).toInt();
            connectorData.route.append(QPoint(x, y));
        }
    }

    connectorData.startNodeIsChild = element.attribute("start-node-is-child", "0").toInt();
    if (connectorData.startNodeIsChild) {
        connectorData.startNodeCId = element.attribute("start-node-cid", QString());
        connectorData.startNodeParent = element.attribute("start-node-parent", QString());
    } else
        connectorData.startNodeId = element.attribute("start-node-id", QString());

    connectorData.endNodeIsChild = element.attribute("end-node-is-child", "0").toInt();
    if (connectorData.endNodeIsChild) {
        connectorData.endNodeCId = element.attribute("end-node-cid", QString());
        connectorData.endNodeParent = element.attribute("end-node-parent", QString());
    } else
        connectorData.endNodeId = element.attribute("end-node-id", QString());

    m_connectorDataMap[id] = connectorData;
}
// END functions for generating / reading QDomElements

QString ItemDocumentData::documentTypeString() const
{
    switch (m_documentType) {
    case Document::dt_circuit:
        return "circuit";
        break;
    case Document::dt_flowcode:
        return "flowcode";
        break;
    case Document::dt_mechanics:
        return "mechanics";
        break;
    case Document::dt_text:
    case Document::dt_none:
    default:
        return "none";
        break;
    }
}

QString ItemDocumentData::revisionString() const
{
    return "1";
}

void ItemDocumentData::saveDocumentState(ItemDocument *itemDocument)
{
    if (!itemDocument)
        return;

    reset();

    addItems(itemDocument->itemList());

    if (ICNDocument *icnd = dynamic_cast<ICNDocument *>(itemDocument)) {
        addConnectors(icnd->connectorList());
        addNodes(icnd->nodeList());

        if (FlowCodeDocument *fcd = dynamic_cast<FlowCodeDocument *>(itemDocument)) {
            if (fcd->microSettings())
                setMicroData(fcd->microSettings()->microData());
        }
    }

    m_documentType = itemDocument->type();
}

void ItemDocumentData::generateUniqueIDs(ItemDocument *itemDocument)
{
    if (!itemDocument)
        return;

    QStringMap replaced;
    replaced[""] = QString();
    replaced[QString()] = QString();

    ItemDataMap newItemDataMap;
    ConnectorDataMap newConnectorDataMap;
    NodeDataMap newNodeDataMap;

    // BEGIN Go through and replace the old ids
    {
        const ItemDataMap::iterator end = m_itemDataMap.end();
        for (ItemDataMap::iterator it = m_itemDataMap.begin(); it != end; ++it) {
            if (!replaced.contains(it.key()))
                replaced[it.key()] = itemDocument->generateUID(it.key());

            newItemDataMap[replaced[it.key()]] = it.value();
        }
    }
    {
        const NodeDataMap::iterator end = m_nodeDataMap.end();
        for (NodeDataMap::iterator it = m_nodeDataMap.begin(); it != end; ++it) {
            if (!replaced.contains(it.key()))
                replaced[it.key()] = itemDocument->generateUID(it.key());

            newNodeDataMap[replaced[it.key()]] = it.value();
        }
    }
    {
        const ConnectorDataMap::iterator end = m_connectorDataMap.end();
        for (ConnectorDataMap::iterator it = m_connectorDataMap.begin(); it != end; ++it) {
            if (!replaced.contains(it.key()))
                replaced[it.key()] = itemDocument->generateUID(it.key());

            newConnectorDataMap[replaced[it.key()]] = it.value();
        }
    }
    // END Go through and replace the old ids

    // BEGIN Go through and replace the internal references to the ids
    {
        const ItemDataMap::iterator end = newItemDataMap.end();
        for (ItemDataMap::iterator it = newItemDataMap.begin(); it != end; ++it) {
            it.value().parentId = replaced[it.value().parentId];
        }
    }
    {
        const ConnectorDataMap::iterator end = newConnectorDataMap.end();
        for (ConnectorDataMap::iterator it = newConnectorDataMap.begin(); it != end; ++it) {
            it.value().startNodeParent = replaced[it.value().startNodeParent];
            it.value().endNodeParent = replaced[it.value().endNodeParent];

            it.value().startNodeId = replaced[it.value().startNodeId];
            it.value().endNodeId = replaced[it.value().endNodeId];
        }
    }
    // END Go through and replace the internal references to the ids

    m_itemDataMap = newItemDataMap;
    m_connectorDataMap = newConnectorDataMap;
    m_nodeDataMap = newNodeDataMap;
}

void ItemDocumentData::translateContents(int dx, int dy)
{
    // BEGIN Go through and replace the old ids
    {
        const ItemDataMap::iterator end = m_itemDataMap.end();
        for (ItemDataMap::iterator it = m_itemDataMap.begin(); it != end; ++it) {
            it.value().x += dx;
            it.value().y += dx;
        }
    }
    {
        const NodeDataMap::iterator end = m_nodeDataMap.end();
        for (NodeDataMap::iterator it = m_nodeDataMap.begin(); it != end; ++it) {
            it.value().x += dx;
            it.value().y += dy;
        }
    }
    {
        const ConnectorDataMap::iterator end = m_connectorDataMap.end();
        for (ConnectorDataMap::iterator it = m_connectorDataMap.begin(); it != end; ++it) {
            const QPointList::iterator routeEnd = it.value().route.end();
            for (QPointList::iterator routeIt = it.value().route.begin(); routeIt != routeEnd; ++routeIt) {
                *routeIt += QPoint(dx / 8, dy / 8);
            }
        }
    }
}

void ItemDocumentData::restoreDocument(ItemDocument *itemDocument)
{
    if (!itemDocument)
        return;

    ICNDocument *icnd = dynamic_cast<ICNDocument *>(itemDocument);
    FlowCodeDocument *fcd = dynamic_cast<FlowCodeDocument *>(icnd);
    if (fcd && !m_microData.id.isEmpty()) {
        fcd->setPicType(m_microData.id);
        fcd->microSettings()->restoreFromMicroData(m_microData);
    }

    mergeWithDocument(itemDocument, false);

    {
        ItemList removeItems = itemDocument->itemList();
        removeItems.removeAll(static_cast<Item *>(nullptr));

        const ItemDataMap::iterator end = m_itemDataMap.end();
        for (ItemDataMap::iterator it = m_itemDataMap.begin(); it != end; ++it)
            removeItems.removeAll(itemDocument->itemWithID(it.key()));

        const ItemList::iterator removeEnd = removeItems.end();
        for (ItemList::iterator it = removeItems.begin(); it != removeEnd; ++it) {
            if ((*it)->canvas() && (*it)->type() != PicItem::typeString())
                (*it)->removeItem();
        }
    }

    if (icnd) {
        {
            NodeList removeNodes = icnd->nodeList();
            removeNodes.removeAll(static_cast<Node *>(nullptr));

            const NodeDataMap::iterator end = m_nodeDataMap.end();
            for (NodeDataMap::iterator it = m_nodeDataMap.begin(); it != end; ++it)
                removeNodes.removeAll(icnd->nodeWithID(it.key()));

            const NodeList::iterator removeEnd = removeNodes.end();
            for (NodeList::iterator it = removeNodes.begin(); it != removeEnd; ++it) {
                if ((*it)->canvas() && !(*it)->isChildNode())
                    (*it)->removeNode();
            }
        }
        {
            ConnectorList removeConnectors = icnd->connectorList();
            removeConnectors.removeAll(static_cast<Connector *>(nullptr));

            const ConnectorDataMap::iterator end = m_connectorDataMap.end();
            for (ConnectorDataMap::iterator it = m_connectorDataMap.begin(); it != end; ++it)
                removeConnectors.removeAll(icnd->connectorWithID(it.key()));

            const ConnectorList::iterator removeEnd = removeConnectors.end();
            for (ConnectorList::iterator it = removeConnectors.begin(); it != removeEnd; ++it) {
                if ((*it)->canvas())
                    (*it)->removeConnectorNoArg();
            }
        }
    }

    itemDocument->flushDeleteList();
}

void ItemDocumentData::mergeWithDocument(ItemDocument *itemDocument, bool selectNew)
{
    if (!itemDocument)
        return;

    ICNDocument *icnd = dynamic_cast<ICNDocument *>(itemDocument);

    // BEGIN Restore Nodes
    if (icnd) {
        const NodeDataMap::iterator nodeEnd = m_nodeDataMap.end();
        for (NodeDataMap::iterator it = m_nodeDataMap.begin(); it != nodeEnd; ++it) {
            if (!icnd->nodeWithID(it.key())) {
                QString id = it.key();
                if (itemDocument->type() == Document::dt_circuit)
                    new JunctionNode(icnd, 270, QPoint(int(it.value().x), int(it.value().y)), &id);

                else if (itemDocument->type() == Document::dt_flowcode)
                    new JunctionFlowNode(icnd, 270, QPoint(int(it.value().x), int(it.value().y)), &id);
            }
        }
        for (NodeDataMap::iterator it = m_nodeDataMap.begin(); it != nodeEnd; ++it) {
            Node *node = icnd->nodeWithID(it.key());
            if (node)
                node->move(it.value().x, it.value().y);
        }
    }
    // END Restore Nodes

    // BEGIN Restore items
    const ItemDataMap::iterator itemEnd = m_itemDataMap.end();
    for (ItemDataMap::iterator it = m_itemDataMap.begin(); it != itemEnd; ++it) {
        if (!it.value().type.isEmpty() && !itemDocument->itemWithID(it.key())) {
            Item *item = itemLibrary()->createItem(it.value().type, itemDocument, false, it.key().toLatin1().data(), false);
            if (item && !itemDocument->isValidItem(item)) {
                qCWarning(KTL_LOG) << "Attempted to create invalid item with id: " << it.key();
                item->removeItem();
                itemDocument->flushDeleteList();
                item = nullptr;
            }
            if (item) {
                // HACK We move the item now before restoreFromItemData is called later, in case it is to be parented
                //(as we don't want to move children)...
                item->move(it.value().x, it.value().y);
            }
        }
    }
    for (ItemDataMap::iterator it = m_itemDataMap.begin(); it != itemEnd; ++it) {
        Item *item = itemDocument->itemWithID(it.key());
        if (!item)
            continue;

        item->restoreFromItemData(it.value());
        item->finishedCreation();
        if (selectNew)
            itemDocument->select(item);
        item->show();
    }
    // END Restore Items

    // BEGIN Restore Connectors
    if (icnd) {
        const ConnectorDataMap::iterator connectorEnd = m_connectorDataMap.end();
        for (ConnectorDataMap::iterator it = m_connectorDataMap.begin(); it != connectorEnd; ++it) {
            if (icnd->connectorWithID(it.key()))
                continue;

            QString id = it.key();
            Node *startNode = nullptr;
            Node *endNode = nullptr;

            if (it.value().startNodeIsChild) {
                CNItem *item = icnd->cnItemWithID(it.value().startNodeParent);
                if (!item)
                    qCCritical(KTL_LOG) << "Unable to find node parent with id: " << it.value().startNodeParent;
                else
                    startNode = item->childNode(it.value().startNodeCId);
            } else
                startNode = icnd->nodeWithID(it.value().startNodeId);

            if (it.value().endNodeIsChild) {
                CNItem *item = icnd->cnItemWithID(it.value().endNodeParent);
                if (!item)
                    qCCritical(KTL_LOG) << "Unable to find node parent with id: " << it.value().endNodeParent;
                else
                    endNode = item->childNode(it.value().endNodeCId);
            } else
                endNode = icnd->nodeWithID(it.value().endNodeId);

            if (!startNode || !endNode) {
                qCCritical(KTL_LOG) << "End and start nodes for the connector do not both exist";
            } else {
                Connector *connector;

                // HACK // FIXME // TODO
                // for some strange reason the lists in the ItemDocument class the ID lists for items
                // get out of sync, so some id's are considered to be registered, but in fact they
                // have no associated items; this causes strange bugs when inserting subcircuits in the circuit.
                // this is just a temporary fix; someone should get to the real cause of this problem and fix
                // ItemDocument
                if (icnd->connectorWithID(id)) {
                    qCWarning(KTL_LOG) << "Unregistering connector with ID: " << id << ". This should not delete any of your connections!";
                }
                icnd->unregisterUID(id);

                // FIXME ICNDocument->type() used
                // FIXME tons of dynamic_cast
                if ((icnd->type() == Document::dt_circuit) || (icnd->type() == Document::dt_pinMapEditor)) {
                    connector = new ElectronicConnector(dynamic_cast<ECNode *>(startNode), dynamic_cast<ECNode *>(endNode), icnd, &id);
                    (dynamic_cast<ECNode *>(startNode))->addConnector(connector);
                    (dynamic_cast<ECNode *>(endNode))->addConnector(connector);
                } else {
                    connector = new FlowConnector(dynamic_cast<FPNode *>(startNode), dynamic_cast<FPNode *>(endNode), icnd, &id);
                    (dynamic_cast<FPNode *>(startNode))->addOutputConnector(connector);
                    (dynamic_cast<FPNode *>(endNode))->addInputConnector(connector);
                }
            }
        }
        for (ConnectorDataMap::iterator it = m_connectorDataMap.begin(); it != connectorEnd; ++it) {
            Connector *connector = icnd->connectorWithID(it.key());
            if (connector) {
                connector->restoreFromConnectorData(it.value());
                if (selectNew)
                    icnd->select(connector);
            }
        }
    }
    // END Restore Connectors

    // This is kind of hackish, but never mind
    if (FlowCodeDocument *fcd = dynamic_cast<FlowCodeDocument *>(itemDocument)) {
        const ItemList fcdItems = fcd->itemList();
        const ItemList::const_iterator fcdItemsEnd = fcdItems.constEnd();
        for (ItemList::const_iterator it = fcdItems.constBegin(); it != fcdItemsEnd; ++it) {
            if (FlowContainer *fc = dynamic_cast<FlowContainer *>(static_cast<Item *>(*it)))
                fc->updateContainedVisibility();
        }
    }
}

void ItemDocumentData::setMicroData(const MicroData &data)
{
    m_microData = data;
}

void ItemDocumentData::addItems(const ItemList &itemList)
{
    const ItemList::const_iterator end = itemList.constEnd();
    for (ItemList::const_iterator it = itemList.constBegin(); it != end; ++it) {
        if (*it && (*it)->canvas() && (*it)->type() != PicItem::typeString())
            addItemData((*it)->itemData(), (*it)->id());
    }
}

void ItemDocumentData::addConnectors(const ConnectorList &connectorList)
{
    const ConnectorList::const_iterator end = connectorList.constEnd();
    for (ConnectorList::const_iterator it = connectorList.constBegin(); it != end; ++it) {
        if (*it && (*it)->canvas()) {
            if ((*it)->startNode() && (*it)->endNode())
                addConnectorData((*it)->connectorData(), (*it)->id());

            else
                qCDebug(KTL_LOG) << " *it=" << *it << " (*it)->startNode()=" << (*it)->startNode() << " (*it)->endNode()=" << (*it)->endNode();
        }
    }
}

void ItemDocumentData::addNodes(const NodeList &nodeList)
{
    const NodeList::const_iterator end = nodeList.constEnd();
    for (NodeList::const_iterator it = nodeList.constBegin(); it != end; ++it) {
        if (*it && (*it)->canvas() && !(*it)->isChildNode())
            addNodeData((*it)->nodeData(), (*it)->id());
    }
}

void ItemDocumentData::addItemData(ItemData itemData, QString id)
{
    if (m_itemDataMap.contains(id)) {
        qCWarning(KTL_LOG) << "Overwriting item: " << id;
    }
    m_itemDataMap[id] = itemData;
}

void ItemDocumentData::addConnectorData(ConnectorData connectorData, QString id)
{
    if (m_connectorDataMap.contains(id)) {
        qCWarning(KTL_LOG) << "Overwriting connector: " << id;
    }
    m_connectorDataMap[id] = connectorData;
}

void ItemDocumentData::addNodeData(NodeData nodeData, QString id)
{
    if (m_nodeDataMap.contains(id)) {
        qCWarning(KTL_LOG) << "Overwriting node: " << id;
    }
    m_nodeDataMap[id] = nodeData;
}
// END class ItemDocumentData

// BEGIN class ItemData
ItemData::ItemData()
{
    x = 0;
    y = 0;
    z = -1;
    angleDegrees = 0;
    flipped = false;
    orientation = -1;
    setSize = false;
}
// END class ItemData

// BEGIN class ConnectorData
ConnectorData::ConnectorData()
{
    manualRoute = false;
    startNodeIsChild = false;
    endNodeIsChild = false;
}
// END class ConnectorData

// BEGIN class NodeData
NodeData::NodeData()
{
    x = 0;
    y = 0;
}
// END class NodeDaata

// BEGIN class PinData
PinData::PinData()
{
    type = PinSettings::pt_input;
    state = PinSettings::ps_off;
}
// END class PinData

// BEGIN class MicroData
MicroData::MicroData()
{
}

void MicroData::reset()
{
    id = QString();
    pinMap.clear();
}
// END class MicroData

// BEGIN class SubcircuitData
SubcircuitData::SubcircuitData()
    : ItemDocumentData(Document::dt_circuit)
{
}

void SubcircuitData::initECSubcircuit(ECSubcircuit *ecSubcircuit)
{
    if (!ecSubcircuit)
        return;

    generateUniqueIDs(ecSubcircuit->itemDocument());

    // Generate a list of the External Connections, sorting by x coordinate
    std::multimap<double, QString> extCon;
    ItemDataMap::iterator itemEnd = m_itemDataMap.end();
    for (ItemDataMap::iterator it = m_itemDataMap.begin(); it != itemEnd; ++it) {
        if (it.value().type == "ec/external_connection")
            extCon.insert(std::make_pair(it.value().x, it.key()));
    }

    // How many external connections do we have?
    ecSubcircuit->setNumExtCon(extCon.size());

    // Sort the connections into the pins of the subcircuit by y coordinate
    std::multimap<double, QString> leftPins;
    std::multimap<double, QString> rightPins;
    int at = 0;
    int size = (extCon.size() / 2) + (extCon.size() % 2);
    const std::multimap<double, QString>::iterator extConEnd = extCon.end();
    for (std::multimap<double, QString>::iterator it = extCon.begin(); it != extConEnd; ++it) {
        if (at < size)
            leftPins.insert(std::make_pair(m_itemDataMap[it->second].y, it->second));
        else
            rightPins.insert(std::make_pair(m_itemDataMap[it->second].y, it->second));
        at++;
    }

    // Remove the external connections (recording their names and associated numerical position)
    int nodeId = 0;
    typedef QMap<QString, int> IntMap;
    IntMap nodeMap;
    const std::multimap<double, QString>::iterator leftPinsEnd = leftPins.end();
    for (std::multimap<double, QString>::iterator it = leftPins.begin(); it != leftPinsEnd; ++it) {
        nodeMap[it->second] = nodeId;
        ecSubcircuit->setExtConName(nodeId, m_itemDataMap[it->second].dataString["name"]);
        nodeId++;
        m_itemDataMap.remove(it->second);
    }
    nodeId = extCon.size() - 1;
    const std::multimap<double, QString>::iterator rightPinsEnd = rightPins.end();
    for (std::multimap<double, QString>::iterator it = rightPins.begin(); it != rightPinsEnd; ++it) {
        nodeMap[it->second] = nodeId;
        ecSubcircuit->setExtConName(nodeId, m_itemDataMap[it->second].dataString["name"]);
        nodeId--;
        m_itemDataMap.remove(it->second);
    }

    // Replace connector references to the old External Connectors to the nodes
    const ConnectorDataMap::iterator connectorEnd = m_connectorDataMap.end();
    for (ConnectorDataMap::iterator it = m_connectorDataMap.begin(); it != connectorEnd; ++it) {
        if (it.value().startNodeIsChild && nodeMap.contains(it.value().startNodeParent)) {
            it.value().startNodeCId = QString::number(nodeMap[it.value().startNodeParent]);
            it.value().startNodeParent = ecSubcircuit->id();
        }
        if (it.value().endNodeIsChild && nodeMap.contains(it.value().endNodeParent)) {
            it.value().endNodeCId = QString::number(nodeMap[it.value().endNodeParent]);
            it.value().endNodeParent = ecSubcircuit->id();
        }
    }

    // Create all the new stuff
    mergeWithDocument(ecSubcircuit->itemDocument(), false);

    // Parent and hide the new stuff
    itemEnd = m_itemDataMap.end();
    for (ItemDataMap::iterator it = m_itemDataMap.begin(); it != itemEnd; ++it) {
        Component *component = static_cast<Component *>(ecSubcircuit->itemDocument()->itemWithID(it.key()));
        if (component) {
            component->setParentItem(ecSubcircuit);
            component->updateConnectorPoints(false);
            component->setVisible(false);
            component->setCanvas(nullptr);
            ecSubcircuit->connect(ecSubcircuit, &ECSubcircuit::subcircuitDeleted, component, &Component::removeItem);
        }
    }
    for (ConnectorDataMap::iterator it = m_connectorDataMap.begin(); it != connectorEnd; ++it) {
        Connector *connector = (static_cast<ICNDocument *>(ecSubcircuit->itemDocument()))->connectorWithID(it.key());
        if (connector) {
            connector->updateConnectorPoints(false);
            connector->setVisible(false);
            connector->setCanvas(nullptr);
            ecSubcircuit->connect(ecSubcircuit, &ECSubcircuit::subcircuitDeleted, connector, &Connector::removeConnectorNoArg);
        }
    }
    const NodeDataMap::iterator nodeEnd = m_nodeDataMap.end();
    for (NodeDataMap::iterator it = m_nodeDataMap.begin(); it != nodeEnd; ++it) {
        Node *node = (static_cast<ICNDocument *>(ecSubcircuit->itemDocument()))->nodeWithID(it.key());
        if (node) {
            node->setVisible(false);
            node->setCanvas(nullptr);
            ecSubcircuit->connect(ecSubcircuit, &ECSubcircuit::subcircuitDeleted, node, qOverload<>(&Node::removeNode));
        }
    }

    ecSubcircuit->doneSCInit();
}
// END class SubcircuitData
