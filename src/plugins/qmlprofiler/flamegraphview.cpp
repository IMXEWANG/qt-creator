/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "flamegraphview.h"
#include "qmlprofilerconstants.h"
#include "qmlprofilertool.h"

#include <tracing/flamegraph.h>
#include <tracing/timelinetheme.h>
#include <utils/theme/theme.h>

#include <QQmlEngine>
#include <QQmlContext>
#include <QVBoxLayout>
#include <QMenu>

namespace QmlProfiler {
namespace Internal {

FlameGraphView::FlameGraphView(QmlProfilerModelManager *manager, QWidget *parent) :
    QmlProfilerEventsView(parent), m_content(new QQuickWidget(this)),
    m_model(new FlameGraphModel(manager, this))
{
    setObjectName("QmlProfiler.FlameGraph.Dock");
    setWindowTitle(tr("Flame Graph"));

#if QT_VERSION < QT_VERSION_CHECK(6, 2, 0)
    qmlRegisterType<FlameGraph::FlameGraph>("QtCreator.Tracing", 1, 0, "FlameGraph");
#endif // Qt < 6.2
    qmlRegisterUncreatableType<FlameGraphModel>("QtCreator.QmlProfiler", 1, 0,
                                                "QmlProfilerFlameGraphModel",
                                                QLatin1String("use the context property"));

    Timeline::TimelineTheme::setupTheme(m_content->engine());

    m_content->rootContext()->setContextProperty(QStringLiteral("flameGraphModel"), m_model);
    m_content->setSource(QUrl(QStringLiteral("qrc:/qmlprofiler/QmlProfilerFlameGraphView.qml")));
    m_content->setClearColor(Utils::creatorTheme()->color(Utils::Theme::Timeline_BackgroundColor1));

    m_content->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    QLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_content);
    setLayout(layout);

    connect(m_content->rootObject(), SIGNAL(typeSelected(int)), this, SIGNAL(typeSelected(int)));
    connect(m_model, &FlameGraphModel::gotoSourceLocation,
            this, &FlameGraphView::gotoSourceLocation);
}

void FlameGraphView::selectByTypeId(int typeIndex)
{
    m_content->rootObject()->setProperty("selectedTypeId", typeIndex);
}

void FlameGraphView::onVisibleFeaturesChanged(quint64 features)
{
    m_model->restrictToFeatures(features);
}

void FlameGraphView::contextMenuEvent(QContextMenuEvent *ev)
{
    QMenu menu;

    QPoint position = ev->globalPos();

    menu.addActions(QmlProfilerTool::profilerContextMenuActions());
    menu.addSeparator();
    QAction *getGlobalStatsAction = menu.addAction(tr("Show Full Range"));
    getGlobalStatsAction->setEnabled(m_model->modelManager()->isRestrictedToRange());
    QAction *resetAction = menu.addAction(tr("Reset Flame Graph"));
    resetAction->setEnabled(m_content->rootObject()->property("zoomed").toBool());

    const QAction *selected = menu.exec(position);
    if (selected == getGlobalStatsAction)
        emit showFullRange();
    else if (selected == resetAction)
        QMetaObject::invokeMethod(m_content->rootObject(), "resetRoot");
}

} // namespace Internal
} // namespace QmlProfiler
