/*
 * Copyright (c) 2006 Justin Knotzke (jknotzke@shampoo.ca)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "DBAccess.h"
#include <QtSql>
#include <QtGui>
#include "RideFile.h"
#include "Zones.h"
#include "Settings.h"
#include "RideItem.h"
#include "RideMetric.h"
#include "TimeUtils.h"
#include <assert.h>
#include <math.h>
#include <QtXml/QtXml>
#include <QFile>
#include <QFileInfo>
#include "SummaryMetrics.h"
#include "RideMetadata.h"
#include "SpecialFields.h"

#include <boost/scoped_array.hpp>
#include <boost/crc.hpp>

// DB Schema Version - YOU MUST UPDATE THIS IF THE SCHEMA VERSION CHANGES!!!
// Schema version will change if a) the default metadata.xml is updated
//                            or b) new metrics are added / old changed
static int DBSchemaVersion = 24;

DBAccess::DBAccess(MainWindow* main, QDir home) : main(main), home(home)
{
	initDatabase(home);

    // check we have one and use built in if not there
    QString filename = main->home.absolutePath()+"/measures.xml";
    if (!QFile(filename).exists()) filename = ":/xml/measures.xml";
    RideMetadata::readXML(filename, mkeywordDefinitions, mfieldDefinitions);

}

void DBAccess::closeConnection()
{
    dbconn.close();
}

DBAccess::~DBAccess()
{
    closeConnection();
}

void
DBAccess::initDatabase(QDir home)
{


    if(dbconn.isOpen()) return;

    QString cyclist = QFileInfo(home.path()).baseName();
    sessionid = QString("%1%2").arg(cyclist).arg(main->session++);

    if (main->session == 1) {
        // first
        main->db = QSqlDatabase::addDatabase("QSQLITE", sessionid);
        main->db.setDatabaseName(home.absolutePath() + "/metricDB");
        //dbconn = db.database(QString("GC"));
        dbconn = main->db.database(sessionid);
    } else {
        // clone the first one!
        dbconn = QSqlDatabase::cloneDatabase(main->db, sessionid);
        dbconn.open();
    }

    if (!dbconn.isOpen()) {
        QMessageBox::critical(0, qApp->translate("DBAccess","Cannot open database"),
                       qApp->translate("DBAccess","Unable to establish a database connection.\n"
                                       "This feature requires SQLite support. Please read "
                                       "the Qt SQL driver documentation for information how "
                                       "to build it.\n\n"
                                       "Click Cancel to exit."), QMessageBox::Cancel);
    } else {

        // create database - does nothing if its already there
        createDatabase();
    }
}

static int
computeFileCRC(QString filename)
{
    QFile file(filename);
    QFileInfo fileinfo(file);

    // open file
    if (!file.open(QFile::ReadOnly)) return 0;

    // allocate space
    boost::scoped_array<char> data(new char[file.size()]);

    // read entire file into memory
    QDataStream *rawstream(new QDataStream(&file));
    rawstream->readRawData(&data[0], file.size());
    file.close();

    // calculate the CRC
    boost::crc_optimal<16, 0x1021, 0xFFFF, 0, false, false> CRC;
    CRC.process_bytes(&data[0], file.size());

    return CRC.checksum();
}

bool DBAccess::createMetricsTable()
{
    SpecialFields sp;
    QSqlQuery query(dbconn);
    bool rc;
    bool createTables = true;

    // does the table exist?
    rc = query.exec("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name;");
    if (rc) {
        while (query.next()) {

            QString table = query.value(0).toString();
            if (table == "metrics") {
                createTables = false;
                break;
            }
        }
    }
    // we need to create it!
    if (rc && createTables) {
        QString createMetricTable = "create table metrics (filename varchar primary key,"
                                    "identifier varchar,"
                                    "timestamp integer,"
                                    "ride_date date,"
                                    "fingerprint integer";

        // Add columns for all the metric factory metrics
        const RideMetricFactory &factory = RideMetricFactory::instance();
        for (int i=0; i<factory.metricCount(); i++)
            createMetricTable += QString(", X%1 double").arg(factory.metricName(i));

        // And all the metadata texts
        foreach(FieldDefinition field, main->rideMetadata()->getFields()) {
            if (!sp.isMetric(field.name) && (field.type < 3)) {
                createMetricTable += QString(", Z%1 varchar").arg(sp.makeTechName(field.name));
            }
        }

        // And all the metadata metrics
        foreach(FieldDefinition field, main->rideMetadata()->getFields()) {
            if (!sp.isMetric(field.name) && (field.type == 3 || field.type == 4)) {
                createMetricTable += QString(", Z%1 double").arg(sp.makeTechName(field.name));
            }
        }
        createMetricTable += " )";

        rc = query.exec(createMetricTable);
        //if (!rc) qDebug()<<"create table failed!"  << query.lastError();

        // add row to version database
        QString metadataXML =  QString(home.absolutePath()) + "/metadata.xml";
        int metadatacrcnow = computeFileCRC(metadataXML);
        QDateTime timestamp = QDateTime::currentDateTime();

        // wipe current version row
        query.exec("DELETE FROM version where table_name = \"metrics\"");

        query.prepare("INSERT INTO version (table_name, schema_version, creation_date, metadata_crc ) values (?,?,?,?)");
        query.addBindValue("metrics");
	    query.addBindValue(DBSchemaVersion);
	    query.addBindValue(timestamp.toTime_t());
	    query.addBindValue(metadatacrcnow);
        rc = query.exec();
    }
    return rc;
}

bool DBAccess::dropMetricTable()
{
    QSqlQuery query("DROP TABLE metrics", dbconn);
    bool rc = query.exec();
    return rc;
}

bool DBAccess::createMeasuresTable()
{
    QSqlQuery query(dbconn);
    bool rc;
    bool createTables = true;

    // does the table exist?
    rc = query.exec("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name;");
    if (rc) {
        while (query.next()) {

            QString table = query.value(0).toString();
            if (table == "measures") {
                createTables = false;
                break;
            }
        }
    }
    // we need to create it!
    if (rc && createTables) {

        // read definitions from measures.xml
        SpecialFields sp; // just for makeTechName function...
        QList<FieldDefinition> fieldDefinitions;
        QList<KeywordDefinition> keywordDefinitions; //NOTE: not used in measures.xml

        // check we have one and use built in if not there
        QString filename = main->home.absolutePath()+"/measures.xml";
        if (!QFile(filename).exists()) filename = ":/xml/measures.xml";
        RideMetadata::readXML(filename, keywordDefinitions, fieldDefinitions);

        QString createMeasuresTable = "create table measures (timestamp integer primary key,"
                                      "measure_date date";

        // And all the metadata texts
        foreach(FieldDefinition field, fieldDefinitions)
            if (field.type < 3) createMeasuresTable += QString(", Z%1 varchar").arg(sp.makeTechName(field.name));

        // And all the metadata measures
        foreach(FieldDefinition field, fieldDefinitions)
            if (field.type == 3 || field.type == 4)
                createMeasuresTable += QString(", Z%1 double").arg(sp.makeTechName(field.name));

        createMeasuresTable += " )";

        rc = query.exec(createMeasuresTable);
        //if (!rc) qDebug()<<"create table failed!"  << query.lastError();

        // add row to version database
        QString measuresXML =  QString(home.absolutePath()) + "/measures.xml";
        int measurescrcnow = computeFileCRC(measuresXML);
        QDateTime timestamp = QDateTime::currentDateTime();

        // wipe current version row
        rc = query.exec("DELETE FROM version where table_name = \"measures\";");

        // add row to version table
        query.prepare("INSERT INTO version (table_name, schema_version, creation_date, metadata_crc ) values (?,?,?,?)");
        query.addBindValue("measures");
	    query.addBindValue(DBSchemaVersion);
	    query.addBindValue(timestamp.toTime_t());
	    query.addBindValue(measurescrcnow);
        rc = query.exec();
    }
    return rc;
}

bool DBAccess::dropMeasuresTable()
{
    QSqlQuery query("DROP TABLE measures", dbconn);
    bool rc = query.exec();
    return rc;
}

bool DBAccess::createDatabase()
{
    // check schema version and if missing recreate database
    checkDBVersion();

    // Ride metrics
	createMetricsTable();

    // Athlete measures
    createMeasuresTable();

    return true;
}

void DBAccess::checkDBVersion()
{
    // get a CRC for metadata.xml
    QString metadataXML =  QString(home.absolutePath()) + "/metadata.xml";
    int metadatacrcnow = computeFileCRC(metadataXML);

    // get a CRC for measures.xml
    QString measuresXML =  QString(home.absolutePath()) + "/measures.xml";
    int measurescrcnow = computeFileCRC(measuresXML);

    // can we get a version number?
    QSqlQuery query("SELECT table_name, schema_version, creation_date, metadata_crc from version;", dbconn);

    bool rc = query.exec();

    if (!rc) {
        // we couldn't read the version table properly
        // it must be out of date!!

        QSqlQuery dropM("DROP TABLE version", dbconn);
        dropM.exec();

        // recreate version table and add one entry
        QSqlQuery version("CREATE TABLE version ( table_name varchar primary key, schema_version integer, creation_date date, metadata_crc integer );", dbconn);
        version.exec();

        // wipe away whatever (if anything is there)
        dropMetricTable();
        dropMeasuresTable();

        // create afresh
        createMetricsTable();
        createMeasuresTable();

        return;
    }

    // ok we checked out ok, so lets adjust db schema to reflect
    // tne current version / crc
    bool dropMetric = false;
    bool dropMeasures = false;

    while (query.next()) {
        QString table_name = query.value(0).toString();
        int currentversion = query.value(1).toInt();
        //int creationdate = query.value(2).toInt(); // not relevant anymore, we use version/crc
        int crc = query.value(3).toInt();

        if (table_name == "metrics" && (currentversion != DBSchemaVersion || crc != metadatacrcnow)) {
            dropMetric = true;
        }

        if (table_name == "measures" && crc != measurescrcnow) {
            dropMeasures = true;
        }
    }
    query.finish();

    // "metrics" table, is it up-to-date?
    if (dropMetric) {
        dropMetricTable();
        createMetricsTable();
    }

    // "measures" table, is it up-to-date? - export - recreate - import ....
    // XXX gets wiped away for now, will fix later
    if (dropMeasures) {
        dropMeasuresTable();
        createMeasuresTable();
    }
}

/*----------------------------------------------------------------------
 * CRUD routines for Metrics table
 *----------------------------------------------------------------------*/
bool DBAccess::importRide(SummaryMetrics *summaryMetrics, RideFile *ride, unsigned long fingerprint, bool modify)
{
    SpecialFields sp;
	QSqlQuery query(dbconn);
    QDateTime timestamp = QDateTime::currentDateTime();

    if (modify) {
        // zap the current row
	    query.prepare("DELETE FROM metrics WHERE filename = ?;");
	    query.addBindValue(summaryMetrics->getFileName());
        query.exec();
    }

    // construct an insert statement
    QString insertStatement = "insert into metrics ( filename, identifier, timestamp, ride_date, fingerprint ";
    const RideMetricFactory &factory = RideMetricFactory::instance();
    for (int i=0; i<factory.metricCount(); i++)
        insertStatement += QString(", X%1 ").arg(factory.metricName(i));

    // And all the metadata texts
    foreach(FieldDefinition field, main->rideMetadata()->getFields()) {
        if (!sp.isMetric(field.name) && (field.type < 3)) {
            insertStatement += QString(", Z%1 ").arg(sp.makeTechName(field.name));
        }
    }
        // And all the metadata metrics
    foreach(FieldDefinition field, main->rideMetadata()->getFields()) {
        if (!sp.isMetric(field.name) && (field.type == 3 || field.type == 4)) {
            insertStatement += QString(", Z%1 ").arg(sp.makeTechName(field.name));
        }
    }

    insertStatement += " ) values (?,?,?,?,?"; // filename, identifier, timestamp, ride_date
    for (int i=0; i<factory.metricCount(); i++)
        insertStatement += ",?";
    foreach(FieldDefinition field, main->rideMetadata()->getFields()) {
        if (!sp.isMetric(field.name) && field.type < 5) {
            insertStatement += ",?";
        }
    }
    insertStatement += ")";

	query.prepare(insertStatement);

    // filename, timestamp, ride date
	query.addBindValue(summaryMetrics->getFileName());
	query.addBindValue(summaryMetrics->getId());
	query.addBindValue(timestamp.toTime_t());
    query.addBindValue(summaryMetrics->getRideDate());
    query.addBindValue((int)fingerprint);

    // values
    for (int i=0; i<factory.metricCount(); i++) {
	    query.addBindValue(summaryMetrics->getForSymbol(factory.metricName(i)));
    }

    // And all the metadata texts
    foreach(FieldDefinition field, main->rideMetadata()->getFields()) {

        if (!sp.isMetric(field.name) && (field.type < 3)) {
            query.addBindValue(ride->getTag(field.name, ""));
        }
    }
    // And all the metadata metrics
    foreach(FieldDefinition field, main->rideMetadata()->getFields()) {

        if (!sp.isMetric(field.name) && (field.type == 3 || field.type == 4)) {
            query.addBindValue(ride->getTag(field.name, "0.0").toDouble());
        } else if (!sp.isMetric(field.name)) {
            if (field.name == "Recording Interval")  // XXX Special - need a better way...
                query.addBindValue(ride->recIntSecs());
        }
    }

    // go do it!
	bool rc = query.exec();

	//if(!rc) qDebug() << query.lastError();

	return rc;
}

bool
DBAccess::deleteRide(QString name)
{
    QSqlQuery query(dbconn);

    query.prepare("DELETE FROM metrics WHERE filename = ?;");
    query.addBindValue(name);
    return query.exec();
}

QList<QDateTime> DBAccess::getAllDates()
{
    QSqlQuery query("SELECT ride_date from metrics ORDER BY ride_date;", dbconn);
    QList<QDateTime> dates;

    query.exec();
    while(query.next())
    {
        QDateTime date = query.value(0).toDateTime();
        dates << date;
    }
    return dates;
}

QList<SummaryMetrics> DBAccess::getAllMetricsFor(QDateTime start, QDateTime end)
{
    SpecialFields sp;
    QList<SummaryMetrics> metrics;

    // null date range fetches all, but not currently used by application code
    // since it relies too heavily on the results of the QDateTime constructor
    if (start == QDateTime()) start = QDateTime::currentDateTime().addYears(-10);
    if (end == QDateTime()) end = QDateTime::currentDateTime().addYears(+10);

    // construct the select statement
    QString selectStatement = "SELECT filename, identifier, ride_date";
    const RideMetricFactory &factory = RideMetricFactory::instance();
    for (int i=0; i<factory.metricCount(); i++)
        selectStatement += QString(", X%1 ").arg(factory.metricName(i));
    foreach(FieldDefinition field, main->rideMetadata()->getFields()) {
        if (!sp.isMetric(field.name) && field.type < 5) {
            selectStatement += QString(", Z%1 ").arg(sp.makeTechName(field.name));
        }
    }
    selectStatement += " FROM metrics where DATE(ride_date) >=DATE(:start) AND DATE(ride_date) <=DATE(:end) "
                       " ORDER BY ride_date;";

    // execute the select statement
    QSqlQuery query(selectStatement, dbconn);
    query.bindValue(":start", start.date());
    query.bindValue(":end", end.date());
    query.exec();
    while(query.next())
    {
        SummaryMetrics summaryMetrics;

        // filename and date
        summaryMetrics.setFileName(query.value(0).toString());
        summaryMetrics.setId(query.value(1).toString());
        summaryMetrics.setRideDate(query.value(2).toDateTime());
        // the values
        int i=0;
        for (; i<factory.metricCount(); i++)
            summaryMetrics.setForSymbol(factory.metricName(i), query.value(i+3).toDouble());
        foreach(FieldDefinition field, main->rideMetadata()->getFields()) {
            if (!sp.isMetric(field.name) && (field.type == 3 || field.type == 4)) {
                QString underscored = field.name;
                summaryMetrics.setForSymbol(underscored.replace("_"," "), query.value(i+3).toDouble());
                i++;
            } else if (!sp.isMetric(field.name) && field.type < 3) {
                QString underscored = field.name;
                // ignore texts for now XXX todo if want metadata from Summary Metrics
                summaryMetrics.setText(underscored.replace("_"," "), query.value(i+3).toString());
                i++;
            }
        }
        metrics << summaryMetrics;
    }
    return metrics;
}

SummaryMetrics DBAccess::getRideMetrics(QString filename)
{
    SpecialFields sp;
    SummaryMetrics summaryMetrics;

    // construct the select statement
    QString selectStatement = "SELECT filename, identifier";
    const RideMetricFactory &factory = RideMetricFactory::instance();
    for (int i=0; i<factory.metricCount(); i++)
        selectStatement += QString(", X%1 ").arg(factory.metricName(i));
    foreach(FieldDefinition field, main->rideMetadata()->getFields()) {
        if (!sp.isMetric(field.name) && field.type < 5) {
            selectStatement += QString(", Z%1 ").arg(sp.makeTechName(field.name));
        }
    }
    selectStatement += " FROM metrics where filename == :filename ;";

    // execute the select statement
    QSqlQuery query(selectStatement, dbconn);
    query.bindValue(":filename", filename);
    query.exec();
    while(query.next())
    {
        // filename and date
        summaryMetrics.setFileName(query.value(0).toString());
        summaryMetrics.setId(query.value(1).toString());
        // the values
        int i=0;
        for (; i<factory.metricCount(); i++)
            summaryMetrics.setForSymbol(factory.metricName(i), query.value(i+2).toDouble());
        foreach(FieldDefinition field, main->rideMetadata()->getFields()) {
            if (!sp.isMetric(field.name) && (field.type == 3 || field.type == 4)) {
                QString underscored = field.name;
                summaryMetrics.setForSymbol(underscored.replace(" ","_"), query.value(i+2).toDouble());
                i++;
            } else if (!sp.isMetric(field.name) && field.type < 3) {
                // ignore texts for now XXX todo if want metadata from Summary Metrics
                QString underscored = field.name;
                summaryMetrics.setText(underscored.replace("_"," "), query.value(i+2).toString());
                i++;
            }
        }
    }
    return summaryMetrics;
}

/*----------------------------------------------------------------------
 * CRUD routines for Measures table
 *----------------------------------------------------------------------*/
bool DBAccess::importMeasure(SummaryMetrics *summaryMetrics)
{
	QSqlQuery query(dbconn);

    // construct an insert statement
    QString insertStatement = "insert into measures (timestamp, measure_date";

    // And all the metadata texts
    foreach(FieldDefinition field, mfieldDefinitions) {
        if (field.type < 3) {
            insertStatement += QString(", Z%1 ").arg(msp.makeTechName(field.name));
        }
    }
        // And all the metadata metrics
    foreach(FieldDefinition field, mfieldDefinitions) {
        if (field.type == 3 || field.type == 4) {
            insertStatement += QString(", Z%1 ").arg(msp.makeTechName(field.name));
        }
    }

    insertStatement += " ) values (?,?"; // timestamp, measure_date

    foreach(FieldDefinition field, mfieldDefinitions) {
        if (field.type < 5) {
            insertStatement += ",?";
        }
    }
    insertStatement += ")";

	query.prepare(insertStatement);

    // timestamp and date
    query.addBindValue(summaryMetrics->getDateTime().toTime_t());
    query.addBindValue(summaryMetrics->getDateTime().date());

    // And all the text measures
    foreach(FieldDefinition field, mfieldDefinitions) {
        if (field.type < 3) {
            query.addBindValue(summaryMetrics->getText(field.name, ""));
        }
    }
    // And all the numeric measures
    foreach(FieldDefinition field, mfieldDefinitions) {
        if (field.type == 3 || field.type == 4) {
            query.addBindValue(summaryMetrics->getText(field.name, "nan").toDouble());
        }
    }
    // go do it!
	bool rc = query.exec();

	//if(!rc) qDebug() << query.lastError();

	return rc;
}

QList<SummaryMetrics> DBAccess::getAllMeasuresFor(QDateTime start, QDateTime end)
{
    SpecialFields sp;
    QList<FieldDefinition> fieldDefinitions;
    QList<KeywordDefinition> keywordDefinitions; //NOTE: not used in measures.xml

    // check we have one and use built in if not there
    QString filename = main->home.absolutePath()+"/measures.xml";
    if (!QFile(filename).exists()) filename = ":/xml/measures.xml";
    RideMetadata::readXML(filename, keywordDefinitions, fieldDefinitions);

    QList<SummaryMetrics> measures;

    // null date range fetches all, but not currently used by application code
    // since it relies too heavily on the results of the QDateTime constructor
    if (start == QDateTime()) start = QDateTime::currentDateTime().addYears(-10);
    if (end == QDateTime()) end = QDateTime::currentDateTime().addYears(+10);

    // construct the select statement
    QString selectStatement = "SELECT timestamp, measure_date";
    foreach(FieldDefinition field, fieldDefinitions) {
        if (!sp.isMetric(field.name) && field.type < 5) {
            selectStatement += QString(", Z%1 ").arg(sp.makeTechName(field.name));
        }
    }
    selectStatement += " FROM measures where DATE(measure_date) >=DATE(:start) AND DATE(measure_date) <=DATE(:end) "
                       " ORDER BY measure_date;";

    // execute the select statement
    QSqlQuery query(selectStatement, dbconn);
    query.bindValue(":start", start.date());
    query.bindValue(":end", end.date());
    query.exec();
    while(query.next())
    {
        SummaryMetrics add;

        // filename and date
        add.setDateTime(query.value(1).toDateTime());
        // the values
        int i=2;
        foreach(FieldDefinition field, fieldDefinitions) {
            if (field.type == 3 || field.type == 4) {
                add.setText(field.name, query.value(i).toString());
                i++;
            } else if (field.type < 3) {
                add.setText(field.name, query.value(i).toString());
                i++;
            }
        }
        measures << add;
    }
    return measures;
}
