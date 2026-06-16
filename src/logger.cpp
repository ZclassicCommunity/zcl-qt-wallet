#include "logger.h"

Logger::Logger(QObject *parent, QString fileName) : QObject(parent) {
    m_showDate = true;

    if (!fileName.isEmpty()) {
        // HARNESS GAP / robustness fix: the AppDataLocation directory does not
        // exist on a fresh profile, so QFile::open(Append|Text) silently fails
        // ("device not open"; the log file is never created). Create the parent
        // directory first, then verify the open actually succeeded so write()
        // is not a permanent silent no-op. (Found by the NFT GUI E2E: the attach
        // / capability log lines never reached any file.)
        QFileInfo fi(fileName);
        QDir().mkpath(fi.absolutePath());
        file = new QFile;
        file->setFileName(fileName);
        if (!file->open(QIODevice::Append | QIODevice::Text)) {
            // Could not open the file log — drop the handle so write() falls
            // back to qDebug-only (stdout/stderr) instead of pretending to log.
            delete file;
            file = nullptr;
        }
    }

    write("=========Startup==========");
}

void Logger::write(const QString &value) {
    QString text = QDateTime::currentDateTime().toString("dd.MM.yyyy hh:mm:ss ") + value;

    // Always mirror to stdout/stderr so an out-of-process watcher (and the E2E
    // harness, which greps the GUI's captured stdout) sees the same line that
    // goes to the QFile log. Previously these lines existed ONLY in the QFile
    // log, which itself failed to open on a fresh profile — so events like the
    // node-attach and the NFT-capability verdict were invisible to any external
    // observer. qDebug is cheap and side-effect-free here.
    qDebug().noquote() << text;

    if (!file)
        return;

    QTextStream out(file);
    out.setCodec("UTF-8");
    out << text << endl;
}

Logger::~Logger() {
    if (file != 0)
        file->close();
}