#pragma once

#include "workstation_job.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QWidget>

#include <cmath>
#include <optional>

namespace PlotColors {
inline const QColor Green("#17865d"), Amber("#b77900"), Slate("#7a8e9c");
}

// 轨迹视图：计划路径按完成进度着色，叠加回程预览与实测轨迹；实时面板与离线回放共用。
class Plot : public QWidget {
public:
    explicit Plot(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(180); }
    const QVector<Point>* points = nullptr;
    int projection = 0;
    int completed = 0;
    int active = 0;
    std::optional<Point> tcp;
    QVector<Point> plannedHome;
    QVector<Point> measured;
    bool showMeasured = true;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(), QColor("#f7fafc"));
        painter.setPen(QColor("#e2e9ef"));
        for (int x = 0; x < width(); x += 24) painter.drawLine(x, 0, x, height());
        for (int y = 0; y < height(); y += 24) painter.drawLine(0, y, width(), y);
        painter.setPen(QColor("#526777"));
        painter.drawText(QRect(12, 8, width() - 24, 25), Qt::AlignLeft, "WORLD / 相对起点 · mm");
        const bool hasProgram = points && points->size() >= 2;
        if (!hasProgram && plannedHome.isEmpty()) {
            painter.drawText(rect(), Qt::AlignCenter, "打开作业后显示轨迹");
            return;
        }
        const auto project = [&](Point p) {
            if (projection == 1) return QPointF(p.x, p.y);
            if (projection == 2) return QPointF(p.x, p.z);
            return QPointF((p.x - p.y) * .866, p.z + (p.x + p.y) * .5);
        };
        auto a = project(hasProgram ? points->first() : plannedHome.first());
        auto b = a;
        const auto extendBounds = [&](Point p) {
            const auto v = project(p);
            a = {std::min(a.x(), v.x()), std::min(a.y(), v.y())};
            b = {std::max(b.x(), v.x()), std::max(b.y(), v.y())};
        };
        if (hasProgram) for (auto p : *points) extendBounds(p);
        for (auto p : plannedHome) extendBounds(p);
        const double scale = std::min((width() - 78) / std::max(1., b.x() - a.x()),
                                      (height() - 76) / std::max(1., b.y() - a.y()));
        const auto screen = [&](Point p) {
            auto v = project(p) - (a + b) / 2;
            return QPointF(width() / 2. + v.x() * scale, height() / 2. - v.y() * scale);
        };
        if (hasProgram) for (int i = 1; i < points->size(); ++i) {
                const QColor color = i == active ? PlotColors::Amber : i <= completed ? PlotColors::Green : PlotColors::Slate;
                painter.setPen(QPen(color, i == active ? 4 : 2, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(screen(points->at(i - 1)), screen(points->at(i)));
            }
        if (plannedHome.size() > 1) {
            QPainterPath route;
            route.moveTo(screen(plannedHome.first()));
            for (int i = 1; i < plannedHome.size(); ++i) route.lineTo(screen(plannedHome[i]));
            painter.setPen(QPen(QColor("#c66c13"), 2, Qt::DashLine, Qt::RoundCap));
            painter.drawPath(route);
        }
        if (showMeasured) {
            QPainterPath trace;
            bool open = false;
            for (const auto& point : measured) {
                if (!std::isfinite(point.x)) { open = false; continue; }
                if (open) trace.lineTo(screen(point));
                else trace.moveTo(screen(point));
                open = true;
            }
            painter.setPen(QPen(QColor("#247cb5"), 1.8, Qt::DashLine));
            painter.drawPath(trace);
        }
        if (tcp) {
            const auto position = screen(*tcp);
            painter.setPen(QPen(QColor("#175991"), 2));
            if (rect().adjusted(9, 9, -9, -9).contains(position.toPoint())) {
                painter.drawEllipse(position, 6, 6);
                painter.drawLine(position - QPointF(9, 0), position + QPointF(9, 0));
                painter.drawLine(position - QPointF(0, 9), position + QPointF(0, 9));
            } else {
                painter.drawText(QRect(12, 8, width() - 24, 25), Qt::AlignRight, "TCP 超出视图");
            }
        }
        painter.setPen(QColor("#526777"));
        painter.drawText(QRect(12, height() - 33, width() - 24, 25), Qt::AlignRight,
                         showMeasured ? "绿 已走   黄 当前   灰 待走   橙虚线 回程预览   蓝 实测   ⊕ TCP" : "绿 已走   黄 当前   灰 待走   橙虚线 回程预览   ⊕ TCP");
    }
};
