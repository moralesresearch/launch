#include "dbmanager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>

#include "extattrs.h"

DbManager::DbManager() : filesystemSupportsExtattr(false)
{

    qDebug() << "DbManager::DbManager()";

    // In order to find out whether it is worth doing costly operations regarding
    // extattrs we check whether the filesystem supports them and only use them if it does.
    // This should help speed up things on Live ISOs where extattrs don't seem to be supported.
    bool ok = false;
    ok = Fm::setAttributeValueInt("/usr", "filesystemSupportsExtattr", true);
    if (ok) {
        filesystemSupportsExtattr = true;
        qDebug() << "Extended attributes are supported on /usr; using them";
    } else {
        qDebug() << "Extended attributes are not supported on /usr\n"
                    "or the command to set them needs 'chmod +s'; system will be slower";
    }

    static const QString _databasePath =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
            + QStringLiteral("/launch/launch.db");

    QDir dir(QFileInfo(_databasePath).dir());
    if (!dir.exists())
        dir.mkpath(QFileInfo(_databasePath).dir().absolutePath());

    m_db = QSqlDatabase::addDatabase("QSQLITE");

    m_db.setDatabaseName(_databasePath);

    if (!m_db.open()) {
        qDebug() << QString("Error: connection with database %1 fail").arg(_databasePath);
    } else {
        // qDebug() << QString("Database %1: connection ok").arg(_databasePath);
        _createTable(); // Creates a table if it doesn't exist. Otherwise, it will use existing
                        // table.
    }
}

DbManager::~DbManager()
{
    if (m_db.isOpen()) {
        m_db.close();
    }
    qDebug() << "DbManager::~DbManager()";
}

bool DbManager::isOpen() const
{
    return m_db.isOpen();
}

bool DbManager::_createTable()
{
    bool success = false;

    QSqlQuery query;
    query.prepare("CREATE TABLE applications(path TEXT PRIMARY KEY);");

    if (!query.exec()) {
        // qDebug() << "Couldn't create the table 'applications': one might already exist.";
        success = false;
    }

    return success;
}

// Read "can-open" file and return its contents as a QString;
// this is used e.g., when the system encounters application bundles
// for the first time, or when the "open" command wants to open
// documents but the filesystem doesn't support extended attributes
// Returns nullptr if no "can-open" file is found in the application bundle
QString DbManager::getCanOpenFromFile(QString canonicalPath)
{
    if (canonicalPath.endsWith(".app")) {
        QString canOpenFilePath = canonicalPath + "/Resources/can-open";
        if (!QFileInfo(canOpenFilePath).isFile())
            return QString();
        QFile f(canOpenFilePath);
        if (!f.open(QFile::ReadOnly | QFile::Text))
            return QString();
        QTextStream in(&f);
        return in.readAll();
    } else if (canonicalPath.endsWith(".desktop")) {
        bool ok = false;
        Fm::getAttributeValueQString(canonicalPath, "can-open", ok);
        if (ok)
            return QString(); // extattr is already set
        // The following removes everything after a ';' which XDG loves to use
        // even though they are comments in .ini files...
        // QSettings desktopFile(canonicalPath, QSettings::IniFormat);
        // QString mime = desktopFile.value("Desktop Entry/MimeType").toString();
        // Hence we have to do it the hard way. Yet another example of why XDG is too complex
        QFile f(canonicalPath);
        QString mime = "";
        f.open(QIODevice::ReadOnly | QIODevice::Text);
        if (f.isOpen()) {
            QTextStream in(&f);
            while (!in.atEnd()) {
                QString getLine = in.readLine().trimmed();
                if (getLine.startsWith("MimeType=")) {
                    mime = getLine.remove(0, 9);
                    continue;
                }
            }
        }
        f.close();

        return mime;
    } else {
        // TODO: AppDir
        return QString();
    }
}

void DbManager::handleApplication(QString path)
{
    QString canonicalPath = QDir(path).canonicalPath();

    if (!(QFileInfo(canonicalPath).isDir() || QFileInfo(canonicalPath).isFile())) {
        qDebug() << canonicalPath << "does not exist, removing from launch.db";
        _removeApplication(canonicalPath);
    } else {
        // qDebug() << "Adding" << canonicalPath << "to launch.db";
        _addApplication(canonicalPath);
        // If extended attributes are not supported, there is nothing else to be done here
        if (!filesystemSupportsExtattr) {
            return;
        }

        // Set 'can-open' extattr if 'can-open' extattr doesn't already exist but 'can-open' file
        // exists
        bool ok = false;
        Fm::getAttributeValueQString(canonicalPath, "can-open", ok);
        if (ok)
            return; // extattr is already set
        QString mime = getCanOpenFromFile(canonicalPath);
        if (mime.isEmpty()) {
            qDebug() << "No 'can-open' file:" << canonicalPath;
            return;
        } else if (mime == "") {
            qDebug() << "Empty 'can-open' file:" << canonicalPath;
            return;
        }
        ok = Fm::setAttributeValueQString(canonicalPath, "can-open", mime);
        if (ok) {
            qDebug() << "Set xattr 'can-open' on" << canonicalPath;
        } else {
            qDebug() << "Cannot set xattr 'can-open' on" << canonicalPath;
        }
    }
}

bool DbManager::_addApplication(const QString &path)
{
    bool success = false;

    if (!path.isEmpty()) {
        QSqlQuery queryAdd;
        queryAdd.prepare("INSERT INTO applications (path) VALUES (:path)");
        queryAdd.bindValue(":path", path);

        if (queryAdd.exec()) {
            success = true;
        }
    } else {
        qDebug() << "add application failed: path cannot be empty";
    }

    return success;
}

bool DbManager::_removeApplication(const QString &path)
{
    bool success = false;

    if (applicationExists(path)) {
        QSqlQuery queryDelete;
        queryDelete.prepare("DELETE FROM applications WHERE path = (:path)");
        queryDelete.bindValue(":path", path);
        success = queryDelete.exec();

        if (!success) {
            qDebug() << "remove application failed: " << queryDelete.lastError();
        }
    }

    return success;
}

// NOTE: Currently we are doing 2 SQL queries to ensure that .desktop files
// are only being used as a last resort. Once we are more smart about prioritizing
// which application candidate to use, we may want to reduce this to having just
// one query here again
QStringList DbManager::allApplications() const
{
    QStringList results;
    QTextStream cout(stdout);
    // Prefer everything but .desktop files
    QSqlQuery query("SELECT * FROM applications WHERE path NOT LIKE '%.desktop'");
    int idName = query.record().indexOf("path");
    while (query.next()) {
        results.append(query.value(idName).toString());
    }
    // Fall back to .desktop files
    QSqlQuery query2("SELECT * FROM applications WHERE path LIKE '%.desktop'");
    int idName2 = query2.record().indexOf("path");
    while (query2.next()) {
        results.append(query2.value(idName2).toString());
    }
    return results;
}

unsigned int DbManager::_numberOfApplications() const
{
    QSqlQuery query("SELECT COUNT(*) as cnt FROM applications");
    if (query.next()) {
        return query.value(0).toInt();
    } else {
        return 0;
    }
}

bool DbManager::applicationExists(const QString &path) const
{
    bool exists = false;

    QSqlQuery checkQuery;
    checkQuery.prepare("SELECT path FROM applications WHERE path = (:path)");
    checkQuery.bindValue(":path", path);

    if (checkQuery.exec()) {
        if (checkQuery.next()) {
            exists = true;
        }
    } else {
        qDebug() << "application exists failed: " << checkQuery.lastError();
    }

    return exists;
}

bool DbManager::removeAllApplications()
{
    bool success = false;

    QSqlQuery removeQuery;
    removeQuery.prepare("DELETE FROM applications");

    if (removeQuery.exec()) {
        success = true;
    } else {
        qDebug() << "remove all applications failed: " << removeQuery.lastError();
    }

    return success;
}
