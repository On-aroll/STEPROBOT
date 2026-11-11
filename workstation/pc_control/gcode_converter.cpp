#include "workstation_job.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QTextStream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("StepGcodeConverter");
    QCommandLineParser parser;
    parser.setApplicationDescription("G-code to STEP SRP/SRD ZIP. Dry-run only; validate the generated project in STEPStudio.");
    parser.addHelpOption();
    parser.addOption({{"i", "input"}, "UTF-8 G-code file", "file"});
    parser.addOption({{"o", "output"}, "Output STEP project ZIP", "file"});
    parser.addOption({"template", "Reviewed native origin/return template ZIP", "file",
        QDir(app.applicationDirPath()).absoluteFilePath("../../SR175_3200_Helix_Ready_25mms_v4.zip")});
    parser.addOption({"rapid-mm-s", "Bounded G0 linear speed, 0.001..25 mm/s", "speed", "5"});
    parser.addOption({"arc-tolerance-mm", "Arc chord tolerance, 0.001..0.1 mm", "error", "0.05"});
    parser.addOption({"omit-process-outputs", "Explicitly omit M3/M4/M5/M11; no process outputs are generated"});
    parser.process(app);
    QTextStream output(stdout), errors(stderr);
    output.setEncoding(QStringConverter::Utf8);
    errors.setEncoding(QStringConverter::Utf8);
    if (!parser.isSet("input") || !parser.isSet("output")) { parser.showHelp(2); }
    bool rapidOk = false, toleranceOk = false;
    const ConversionSettings settings{parser.value("rapid-mm-s").toDouble(&rapidOk), parser.value("arc-tolerance-mm").toDouble(&toleranceOk), parser.isSet("omit-process-outputs")};
    if (!rapidOk || !toleranceOk) { errors << "Invalid conversion parameter.\n"; return 2; }
    const auto job = readGcodeJob(parser.value("input"), readNativeJob(parser.value("template")), settings);
    if (!job.runnable()) { errors << conversionReport(job); return 3; }
    const auto error = exportStepJob(job, parser.value("output"));
    if (!error.isEmpty()) { errors << error << '\n'; return 4; }
    output << conversionReport(job) << "ZIP: " << QDir::toNativeSeparators(parser.value("output")) << '\n';
    return 0;
}
