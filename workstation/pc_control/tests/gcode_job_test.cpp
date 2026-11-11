#include "../workstation_job.h"

#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char* reason) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << reason << '\n';
    }
}

Job baseTemplate() {
    Job job;
    job.reviewed = true;
    job.project = "GcodePreview.sr";
    job.files.insert("_project.srd", "BOOL homeSaved := FALSE\n");
    job.files.insert("capture_home.srd", "\n");
    job.files.insert("capture_home.srp", "\n");
    return job;
}

bool allSkipped(const Job& job, int expected) {
    return static_cast<int>(std::count_if(job.steps.cbegin(), job.steps.cend(),
        [](const Step& step) { return step.skipped; })) == expected;
}

}  // namespace

int main() {
    const Job base = baseTemplate();

    // 绝对坐标直线 + 结束。
    const auto valid = compileGcodeJob("valid.nc", {"G21 G17 G90 G94 G91.1", "G0 X1", "G1 X2 F300", "M30"}, 1, base);
    check(valid.runnable(), "valid linear job compiles");
    check(valid.error.isEmpty() && valid.diagnostics.isEmpty(), "valid job has no diagnostics");
    check(valid.points.size() == 3, "two moves produce three path points");
    check(valid.points[1].x == 1 && valid.points[2].x == 2, "absolute coordinates honoured");
    check(valid.files.size() == 5, "generated package has five native files");
    check(valid.steps.size() == 2, "two motion steps recorded");

    // 模态运动 + 增量/绝对混合。
    const auto mixed = compileGcodeJob("mixed.nc", {"G91", "G1 F600 X1", "Y1", "G90 X5", "M2"}, 1, base);
    check(mixed.runnable(), "modal and relative moves compile");
    check(mixed.points.size() == 4, "modal continuation adds points");
    check(std::abs(mixed.points[3].x - 5) < 1e-9 && std::abs(mixed.points[3].y - 1) < 1e-9, "relative then absolute target correct");

    // F 进给范围。
    check(!compileGcodeJob("fast.nc", {"G90", "G1 X1 F1600", "M30"}, 1, base).runnable(), "feed above 1500 mm/min rejected");
    check(!compileGcodeJob("slow.nc", {"G90", "G1 X1 F0.01", "M30"}, 1, base).runnable(), "feed below 0.06 mm/min rejected");

    // 必须拒绝的方言外指令。
    const QStringList rejected[] = {
        {"G20", "G1 X1 F300", "M30"},            // 英制
        {"G90", "G1 X2 F300", "G2 X3 Y0 R1", "M30"},  // R 圆弧
        {"G90", "G41 G1 X1 F300", "M30"},        // 刀具补偿
        {"G90", "M3 S500", "G1 X1 F300", "M30"}, // 工艺输出未勾选忽略
        {"G90", "M30", "G1 X1 F300"},            // M30 后残留指令
        {"G90", "G1 X1 X2 F300", "M30"},         // 重复参数
        {"G90 G91", "G1 X1 F300", "M30"},        // 同组模态冲突
        {"G90", "G1 X1 F300 !", "M30"},          // 无法识别的字符
        {"G90", "M29", "G1 X1 F300", "M30"},     // 未接入的 M 码
        {"G90", "M30"},                          // 没有运动
        {"N-1 G90", "G1 X1 F300", "M30"},        // 负行号
        {"G90", "G1 X1 F300", "G4 P301", "M30"}, // 单次等待超 60 秒
        {"G90", "G1 X2 F300", "G2 X3 Y0 I-1 J0", "M30"},  // 圆弧起终点半径不一致
        {"G90", "G2 X0.05 Y0 I0.005 J0 F300", "M30"},     // 半径低于 0.01
        {"G90", "G2 X202 Y0 I-101 J0 F300", "M30"},       // 半径高于 100
        {"G91", "G1 F600 X1001", "M30"},         // 位移超 ±1000 mm
        {"G90", "G1 X1"},                        // 没有 M2/M30 结束
    };
    for (const auto& source : rejected) {
        const auto job = compileGcodeJob("rejected.nc", source, 1, base);
        if (job.runnable()) {
            ++failures;
            std::cerr << "FAIL: dangerous input accepted: " << source.join(' ').toStdString() << '\n';
        }
    }

    // 未核对的模板必须拒绝转换。
    Job unreviewed;
    check(!compileGcodeJob("g.nc", {"G90", "G1 X1 F300", "M30"}, 1, unreviewed).runnable(), "unreviewed template refuses conversion");

    // M3/M5 在勾选忽略后生成空跑,并标记 skipped。
    const auto omitted = compileGcodeJob("dry.nc", {"G90", "M3 S800", "G1 Z1 F300", "M5", "M30"}, 1, base, {5, .05, true});
    check(omitted.runnable(), "ignored process outputs compile as dry run");
    check(allSkipped(omitted, 2), "two process rows marked skipped");
    check(omitted.steps[0].skipped && !omitted.steps[1].skipped, "skip flags attach to the right rows");

    // 整圆:圆心 (2,0) 半径 1,72 段离散,圆上点误差在弦高容差内。
    const auto circle = compileGcodeJob("circle.nc", {"G90", "G1 X3 F600", "G2 X3 Y0 I-1 J0", "M30"}, 1, base);
    check(circle.runnable(), "full circle compiles");
    check(circle.points.size() == 74, "circle discretises to 72 segments");
    for (int i = 2; i < circle.points.size(); ++i) {
        const double radius = std::hypot(circle.points[i].x - 2, circle.points[i].y);
        if (std::abs(radius - 1) > 0.05 + 1e-9) {
            ++failures;
            std::cerr << "FAIL: circle point " << i << " radius " << radius << '\n';
            break;
        }
    }

    // 螺旋:Z 沿圈线性插值到终点高度。首个 G1 占据 points[1],半圈点在 points[37]。
    const auto helix = compileGcodeJob("helix.nc", {"G90", "G1 X3 F600", "G2 X3 Y0 I-1 J0 Z2", "M30"}, 1, base);
    check(helix.runnable() && std::abs(helix.points.last().z - 2) < 1e-9, "helix ends at target Z");
    check(std::abs(helix.points[37].z - 1) < 1e-9, "helix midpoint reaches half height");

    // SRP 行范围互不重叠且按顺序。
    const auto mapped = compileGcodeJob("mapped.nc", {"G90", "M3 S800", "G1 X1 F300", "G4 P0.5", "G1 X2", "M30"}, 1, base, {5, .05, true});
    check(mapped.runnable() && mapped.steps.size() == 4, "mapped job records four steps");
    int previous = 0;
    for (const auto& step : mapped.steps) {
        if (step.firstLine <= previous || step.lastLine < step.firstLine) {
            ++failures;
            std::cerr << "FAIL: SRP line ranges overlap or invert\n";
            break;
        }
        previous = step.lastLine;
    }
    check(std::abs(mapped.steps[2].dwellSeconds - 0.5) < 1e-9, "G4 P seconds recorded");

    // 1000 点上限:正好 1000 段可行,第 1001 段拒绝。
    QStringList full{"G91", "G1 F600 X0.001"};
    for (int i = 1; i < 1000; ++i) full << "X0.001";
    full << "M30";
    const auto exact = compileGcodeJob("exact.nc", full, 1, base);
    check(exact.runnable() && exact.points.size() == 1001, "exactly 1000 path points accepted");
    auto over = full;
    over.insert(over.size() - 1, "X0.001");
    check(!compileGcodeJob("over.nc", over, 1, base).runnable(), "1001st path point rejected");

    // 32 KiB 输入上限。
    const auto huge = compileGcodeJob("huge.nc", {"G90", "(comment " + QString(33000, 'x') + ")", "G1 X1 F300", "M30"}, 1, base);
    check(!huge.runnable() && huge.error.contains("32 KiB"), "oversized source rejected");

    // BOM:compileGcodeJob 本身仍拒绝(剥离发生在读取入口);读取入口剥离 BOM 后可正常转换,原始字节保留归档。
    check(!compileGcodeJob("bom.nc", {QString(QChar(0xFEFF)) + "G90", "G1 X1 F300", "M30"}, 1, base).runnable(),
          "compile entry alone still rejects BOM text");
    QTemporaryDir folder;
    const auto path = folder.filePath("bom.nc");
    const QByteArray content = "\xEF\xBB\xBF" + QString("G90\nG1 X1 F300\nM30\n").toUtf8();
    { QFile out(path); out.open(QIODevice::WriteOnly); out.write(content); }
    const auto bom = readGcodeJob(path, base);
    check(bom.runnable() && bom.points.size() == 2, "UTF-8 BOM is stripped on import");
    check(bom.inputBytes == content, "original bytes preserved for archive");

    // 起始行之前的运动只重新定位,不生成路径点;路径点按起始行位置取偏移。
    const auto fromMid = compileGcodeJob("mid.nc", {"G90", "G1 X5 F300", "G1 X1", "M30"}, 3, base);
    check(fromMid.runnable() && fromMid.points.size() == 2, "rows before start reposition without path points");
    check(std::abs(fromMid.points[0].x) < 1e-9 && std::abs(fromMid.points[1].x + 4) < 1e-9, "path offsets are relative to the start row");

    std::cout << "gcode_job_test: " << (failures ? "FAILURES" : "all checks passed") << " (" << failures << ")\n";
    return failures ? 1 : 0;
}
