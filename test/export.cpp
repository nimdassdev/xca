/* vi: set sw=4 ts=4:
 *
 * Copyright (C) 2024 Christian Hohnstaedt.
 *
 * All rights reserved.
 */

#include <QTest>
#include <QDir>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "lib/pki_multi.h"
#include "lib/db_x509.h"
#include "lib/db_temp.h"
#include "lib/pki_x509.h"
#include "lib/pki_temp.h"
#include "lib/xfile.h"
#include "lib/database_model.h"

#include <widgets/MainWindow.h>
#include "main.h"

void check_pems(const QString &name, int n, QStringList matches = QStringList())
{
	int begin = 0, end = 0;
	qDebug() << "Expecting" << n << "PEMs in" << name;

#if 0
	// This is an endless loop: open_read() succeeds,
	// but isOpen returns false. Stop investigating, use POSIX open()
	XFile F(name);
	while (!F.isOpen()) {
		qDebug() << "OPEN" << name;
		F.close();
		Q_ASSERT(F.open_read());
	}
	QByteArray all = F.readAll();
#else
	int fd = open(qPrintable(name), O_RDONLY);
	Q_ASSERT(fd != -1);
	char buf[65536];
	ssize_t ret = read(fd, buf, sizeof buf);
	Q_ASSERT(ret != -1);
	QByteArray all(buf, ret);
	close(fd);
#endif
	qDebug() << "ALL" << name << all.size();

	foreach(QByteArray b, all.split('\n')) {
		if (b.indexOf("-----BEGIN ") == 0)
			begin++;
		if (b.indexOf("-----END ") == 0)
			end++;

		QMutableStringListIterator i(matches);
		while (i.hasNext()) {
			QByteArray match = i.next().toUtf8();
			if (b.indexOf(match) != -1)
				i.remove();
		}
    }
	QCOMPARE(begin, n);
	QCOMPARE(end, n);
	foreach(QString m, matches) {
		qDebug() << QString("Pattern %1 not found in %2").arg(m).arg(name);
	}
	QCOMPARE(matches.size(), 0);
}

void verify_key(const QString &name, QList<unsigned> hashes, bool priv)
{
	pki_multi *pems = new pki_multi();
	QVERIFY(pems != nullptr);
	pems->probeAnything(name);
	QCOMPARE(pems->get().size(), hashes.size());
	foreach (pki_base *pki, pems->get()) {
		unsigned hash = pki->hash();
		qDebug() << pki->getIntName() << hash;
		QVERIFY2(hashes.contains(hash),
			qPrintable(QString("%1 not expected in %2 (%3)")
				.arg(pki->getIntName())
				.arg(name).arg(hash)
			)
		);
		pki_key *key = dynamic_cast<pki_key*>(pki);
		if (key) {
			QCOMPARE(key->isPrivKey(), priv);
		}
	}
}

void verify_file(const QString &name, QList<unsigned> hashes)
{
	verify_key(name, hashes, false);
}

void verify_template(const QString &name)
{
	pki_multi *pems = new pki_multi();
	pems->probeAnything(name);
	QList<pki_base *> temps = pems->pull();
	QCOMPARE(temps.size(), 1);
	pki_temp *temp = dynamic_cast<pki_temp*>(temps[0]);
	QVERIFY(temp != nullptr);
	QCOMPARE(temp->getIntName(), "My Template Internal Name");
	QCOMPARE(temp->getComment(), "My XCA TEMPLATE comment");
	x509name xn = temp->getSubject();
	QCOMPARE(xn.getEntryByNid(NID_commonName), "CA Template");
	QCOMPARE(xn.getEntryByNid(NID_pkcs9_emailAddress), "mail@address.to");
}

void export_by_id(int id, const QString &name,
				QModelIndexList &list, db_base *db)
{
	const pki_export *xport = pki_export::by_id(id);
	QVERIFY(xport != nullptr);
	XFile F(name);
	F.open_write();
	if (xport->match_all(F_PEM)) {
		QString prefix = QString("%1\n").arg(xport->help);
		foreach (QModelIndex idx, list) {
			pki_base *pki = db_base::fromIndex(idx);
			QVERIFY(pki != nullptr);
			prefix += QString(" - %1[%2]\n")
					.arg(pki->getIntName())
					.arg(pki->getTypeString());
		}
		F.write(prefix.toUtf8());
	}
	db->exportItems(list, xport, F);
	F.close();
}

void test_main::exportFormat()
{
	int l=0;
	QModelIndex idx;
	QModelIndexList list;
	QDir d; d.mkpath("testdata");

	try {

	ign_openssl_error();
	openDB();
	dbstatus();
	pki_multi *pem = new pki_multi();
	QString all = pemdata["Inter CA 1"] +
				pemdata["Inter CA 1 Key"] +
				pemdata["Root CA"] +
				pemdata["Endentity"] +
				pemdata["CA CRL Test"] +
				pemdata["XCA Template"];

	pem->fromPEMbyteArray(all.toUtf8(), QString());
	QCOMPARE(pem->failed_files.count(), 0);
	Database.insert(pem);
	dbstatus();

	db_base *certs = Database.model<db_x509>();
	QVERIFY(certs != nullptr);

	// Root CA as only item: No chain, no private key
	idx = certs->index(certs->getByName("Root CA"));
	list << idx;
	QCOMPARE(certs->exportFlags(idx) , F_CHAIN | F_PRIVATE);
	QCOMPARE(certs->exportFlags(list) , F_CHAIN | F_PRIVATE | F_MULTI);

	// Inter CA 1: All export options permitted
	// Together with "Root CA" in "list": No chain, private or single
	idx = certs->index(certs->getByName("Inter CA 1"));
	list << idx;
	QCOMPARE(certs->exportFlags(idx) , 0);
	QCOMPARE(certs->exportFlags(list) , F_CHAIN | F_PRIVATE | F_SINGLE);

	// Endentity has no private key and id no CA
	idx = certs->index(certs->getByName("Endentity"));
	list << idx;
	QVERIFY(idx.isValid());
	QCOMPARE(certs->exportFlags(idx) , F_PRIVATE | F_CA);

	pki_key *key = new pki_evp();
	key->fromPEMbyteArray(pemdata["Endentity Key"].toUtf8(), QString());
	openssl_error();
	Database.insert(key);
	dbstatus();

	// Endentity now has a private key, but is still no CA
	QCOMPARE(certs->exportFlags(idx) , F_CA);

#define ROOT_HASH 531145749
#define INTER_HASH 376625776
#define END_HASH 94304590
#define EXPIRED_HASH 1359605174
#define ENDKEY_HASH 1121702347
#define ED25519_HASH 318722247

#define xstr(s) str(s)
#define str(s) #s
#define AUTOFILE(type) "testdata/" # type "_Line" xstr(__LINE__) ".data" ;

	const char *file = AUTOFILE(ALLCERT)
	// Export All certs in one PEM File
	export_by_id(3, file, list, certs);
	verify_file(file, QList<unsigned> { ROOT_HASH, INTER_HASH, END_HASH });
	check_pems(file, 3);
	// Export 2 cert Chain from Inter CA1
	file = AUTOFILE(CERTCHAIN)
	list.clear();
	list << certs->index(certs->getByName("Inter CA 1"));
	export_by_id(2, file, list, certs);
	verify_file(file, QList<unsigned> { ROOT_HASH, INTER_HASH });
	check_pems(file, 2);

	// Export 3 cert Chain from Endentity
	file = AUTOFILE(CERTCHAIN)
	list.clear();
	list << certs->index(certs->getByName("Endentity"));
	export_by_id(2, file, list, certs);
	verify_file(file, QList<unsigned> { ROOT_HASH, INTER_HASH, END_HASH });
	check_pems(file, 3);

	// Export Endentity + corresponding key
	file = AUTOFILE(CERTKEY)
	export_by_id(6, file, list, certs);
	verify_key(file, QList<unsigned> { END_HASH, ENDKEY_HASH }, true);
	check_pems(file, 2, QStringList { " RSA PRIVATE KEY-", " CERTIFICATE-" });

	// Export Endentity + corresponding PKCS#8 key
	file = AUTOFILE(CERTPK8)
	pwdialog->setExpectations(QList<pw_expect*>{
		new pw_expect("pass", pw_ok),
		new pw_expect("pass", pw_ok),
	});
	export_by_id(7, file, list, certs);
	verify_key(file, QList<unsigned> { END_HASH, ENDKEY_HASH }, true);
	check_pems(file, 2, QStringList { " ENCRYPTED PRIVATE KEY-", " CERTIFICATE-" });
	// Export OpenVPN format
	file = AUTOFILE(OPENVPN)
	export_by_id(4, file, list, certs);
	verify_key(file, QList<unsigned> {
			ROOT_HASH, INTER_HASH, END_HASH, ENDKEY_HASH }, true);
	check_pems(file, 5, QStringList { " RSA PRIVATE KEY-",
		" CERTIFICATE-", " CERTIFICATE-"," CERTIFICATE-",
		"<ca>", "</ca>", "<extra-certs>", "</extra-certs>",
		"<cert>", "</cert>", "<key>", "</key>",
		"<tls-auth>", "</tls-auth>" });
	// Export Endentity as PKCS#7
	file = AUTOFILE(CERTP7)
	export_by_id(8, file, list, certs);
	verify_file(file, QList<unsigned> {  END_HASH });
	check_pems(file, 0);
	// Export Endentity as PKCS#7 chain
	file = AUTOFILE(CERTP7)
	export_by_id(12, file, list, certs);
	verify_file(file, QList<unsigned> { ROOT_HASH, INTER_HASH, END_HASH });
	check_pems(file, 0);
	// Export Endentity as DER certificate
	file = AUTOFILE(CERTDER)
	export_by_id(13, file, list, certs);
	verify_file(file, QList<unsigned> {  END_HASH });
	check_pems(file, 0);
	// Export Endentity as OpenVPN config file
	file = AUTOFILE(OPENVPNTA)
	export_by_id(39, file, list, certs);
	check_pems(file, 1, QStringList {
		"BEGIN OpenVPN Static key V1", "END OpenVPN Static key V1" });

	// Revoke endentity
	pki_x509 *endentity = dynamic_cast<pki_x509*>(certs->getByName("Endentity"));
	QVERIFY(endentity != nullptr);
	x509rev rev;
	rev.setSerial(endentity->getSerial());
	rev.setDate(a1time::now());
	endentity->setRevoked(rev);
	QVERIFY(endentity->isRevoked());
	// List must not be empty, but may contain anything
	list.clear();
	list << certs->index(certs->getByName("Inter CA 1"));
	// Export unusable as PEM
	file = AUTOFILE(UNUSABLEPEM)
	export_by_id(40, file, list, certs);
	verify_file(file, QList<unsigned> { END_HASH, EXPIRED_HASH });
	check_pems(file, 2);
	// Once more as PKCS#7
	file = AUTOFILE(UNUSABLEP7)
	export_by_id(41, file, list, certs);
	verify_file(file, QList<unsigned> { END_HASH, EXPIRED_HASH });
	check_pems(file, 0);

	///////////////////////////////

	// Export Endentity key
	list.clear();
	key = dynamic_cast<pki_x509*>(certs->getByName("Endentity"))->getRefKey();
	db_base *keys = Database.model<db_key>();
	list << keys->index(key);

	// Public Key
	file = AUTOFILE(PUBKEY)
	export_by_id(19, file, list, keys);
	verify_key(file, QList<unsigned> {  ENDKEY_HASH }, false);
	check_pems(file, 1, QStringList{ "PUBLIC KEY" });

	// Private Key
	file = AUTOFILE(PRIVKEY)
	export_by_id(20, file, list, keys);
	verify_key(file, QList<unsigned> {  ENDKEY_HASH }, true);
	check_pems(file, 1, QStringList{ "RSA PRIVATE KEY" });

	// Private Key Openssl Encrypted
	file = AUTOFILE(PRIVKEY)
	pwdialog->setExpectations(QList<pw_expect*>{
		new pw_expect("pass", pw_ok),
		new pw_expect("pass", pw_ok),
	});
	export_by_id(21, file, list, keys);
	verify_key(file, QList<unsigned> {  ENDKEY_HASH }, true);
	check_pems(file, 1, QStringList { "DEK-Info: ", "Proc-Type: 4,ENCRYPTED", "BEGIN RSA PRIVATE KEY" });

	// Private SSH Key
	file = AUTOFILE(PRIVSSH)
	export_by_id(22, file, list, keys);
	verify_key(file, QList<unsigned> {  ENDKEY_HASH }, true);
	check_pems(file, 1, QStringList{ "RSA PRIVATE KEY" });

	// Public SSH Key
	file = AUTOFILE(PUBSSH)
	export_by_id(23, file, list, keys);
	verify_key(file, QList<unsigned> {  ENDKEY_HASH }, false);
	check_pems(file, 0, QStringList{ "ssh-rsa " });

	// Public DER Key
	file = AUTOFILE(PUBDER)
	export_by_id(24, file, list, keys);
	verify_key(file, QList<unsigned> {  ENDKEY_HASH }, false);
	check_pems(file, 0);

	// Private DER Key
	file = AUTOFILE(PRIVDER)
	export_by_id(25, file, list, keys);
	verify_key(file, QList<unsigned> {  ENDKEY_HASH }, true);
	check_pems(file, 0);

	// Private PVK Key
	file = AUTOFILE(PVK)
	export_by_id(26, file, list, keys);
	verify_key(file, QList<unsigned> {  ENDKEY_HASH }, true);
	check_pems(file, 0);

	// Import ED25519 Key
	key = new pki_evp();
	key->fromPEMbyteArray(pemdata["ED25519 Key"].toUtf8(), QString("ED25519 Key"));
	openssl_error();
	Database.insert(key);
	dbstatus();

	list.clear();
	key = dynamic_cast<pki_key*>(keys->getByName("ED25519 Key"));
	list << keys->index(key);

	// Export ED25519 as Private SSH Key
	file = AUTOFILE(ED25519PRIVSSH)
	export_by_id(22, file, list, keys);
	verify_key(file, QList<unsigned> { ED25519_HASH }, true);
	check_pems(file, 1, QStringList{ "BEGIN OPENSSH PRIVATE KEY" });

	// Export ED25519 as unencrypted PEM Private Key
	file = AUTOFILE(ED25519PRIVPEM)
	export_by_id(20, file, list, keys);
	verify_key(file, QList<unsigned> { ED25519_HASH }, true);
	check_pems(file, 1, QStringList{ "BEGIN PRIVATE KEY" });

	// Export ED25519 as unencrypted PKCS#8 Key (Same output as above)
	file = AUTOFILE(ED25519PRIVPKCS8)
	export_by_id(29, file, list, keys);
	verify_key(file, QList<unsigned> { ED25519_HASH }, true);
	check_pems(file, 1, QStringList{ "BEGIN PRIVATE KEY" });

	// Export ED25519 as encrypted PKCS#8 Key
	file = AUTOFILE(ED25519PRIVPKCS8ENC)
	pwdialog->setExpectations(QList<pw_expect*>{
		new pw_expect("pass", pw_ok),
		new pw_expect("pass", pw_ok),
	});
	export_by_id(28, file, list, keys);
	verify_key(file, QList<unsigned> { ED25519_HASH }, true);
	check_pems(file, 1, QStringList{ "BEGIN ENCRYPTED PRIVATE KEY" });

	// Export XCA Template and verify the internal name and comment
	file = AUTOFILE(XCA_TEMPLATE)
	db_base *temps = Database.model<db_temp>();
	list.clear();
	pki_base *temp = temps->getByName("CA Template"); // The common name
	Q_ASSERT(temp != nullptr);
	Q_ASSERT(temp->getComment().isEmpty());
	temp->setComment("My XCA TEMPLATE comment");
	temp->setIntName("My Template Internal Name");
	list << temps->index(temp);
	export_by_id(35, file, list, temps);
	verify_template(file);
	check_pems(file, 1, QStringList{ "BEGIN XCA TEMPLATE" });

	} catch (...) {
		QString m = QString("Exception thrown L %1").arg(l);
		QVERIFY2(false, m.toUtf8().constData());
	}
}
