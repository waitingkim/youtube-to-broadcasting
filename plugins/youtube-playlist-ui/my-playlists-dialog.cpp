#include "my-playlists-dialog.hpp"

#include <obs-module.h>
#include <obs.hpp>

#include <QApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTreeWidget>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <thread>

static inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static OBSDataAutoRelease OAuthStatus()
{
	calldata_t cd = {};
	OBSDataAutoRelease d;
	if (proc_handler_call(obs_get_proc_handler(), "youtube_playlist_oauth_status", &cd))
		d = obs_data_create_from_json(calldata_string(&cd, "json"));
	calldata_free(&cd);
	return d;
}

/* Run `work` on a background thread and `done` on the UI thread, unless the
 * dialog has been closed meanwhile. */
template<typename Result>
static void RunAsync(QObject *owner, std::function<Result()> work, std::function<void(const Result &)> done)
{
	QPointer<QObject> guard(owner);
	std::thread([guard, work, done]() {
		Result r = work();
		QMetaObject::invokeMethod(
			qApp,
			[guard, done, r]() {
				if (guard)
					done(r);
			},
			Qt::QueuedConnection);
	}).detach();
}

static QString RandomToken(int length)
{
	static const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
	QString s;
	for (int i = 0; i < length; i++)
		s.append(QChar(chars[QRandomGenerator::system()->bounded((int)sizeof(chars) - 1)]));
	return s;
}

enum { ColTitle, ColCount, ColPrivacy };

MyPlaylistsDialog::MyPlaylistsDialog(QWidget *parent, AddFn add_) : QDialog(parent), add(std::move(add_))
{
	setWindowTitle(T("MyPlaylists.Title"));
	resize(560, 480);

	status = new QLabel(this);
	status->setWordWrap(true);
	configButton = new QPushButton(T("MyPlaylists.Configure"), this);
	loginButton = new QPushButton(T("MyPlaylists.Login"), this);
	logoutButton = new QPushButton(T("MyPlaylists.Logout"), this);
	reloadButton = new QPushButton(T("MyPlaylists.Reload"), this);
	addButton = new QPushButton(T("MyPlaylists.Add"), this);
	addButton->setDefault(true);

	list = new QTreeWidget(this);
	list->setColumnCount(3);
	list->setHeaderLabels({T("Column.Title"), T("MyPlaylists.Count"), T("MyPlaylists.Privacy")});
	list->setRootIsDecorated(false);
	list->header()->setSectionResizeMode(ColTitle, QHeaderView::Stretch);
	list->header()->setSectionResizeMode(ColCount, QHeaderView::ResizeToContents);
	list->header()->setSectionResizeMode(ColPrivacy, QHeaderView::ResizeToContents);
	for (int c = 0; c < 3; c++)
		list->headerItem()->setTextAlignment(c, Qt::AlignCenter);

	auto *account = new QHBoxLayout;
	account->addWidget(configButton);
	account->addStretch(1);
	account->addWidget(loginButton);
	account->addWidget(logoutButton);

	auto *bottom = new QHBoxLayout;
	bottom->addWidget(reloadButton);
	bottom->addStretch(1);
	auto *close = new QPushButton(T("MyPlaylists.Close"), this);
	bottom->addWidget(close);
	bottom->addWidget(addButton);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(status);
	layout->addLayout(account);
	layout->addWidget(list, 1);
	layout->addLayout(bottom);

	connect(configButton, &QPushButton::clicked, this, &MyPlaylistsDialog::ConfigureClient);
	connect(loginButton, &QPushButton::clicked, this, &MyPlaylistsDialog::Login);
	connect(logoutButton, &QPushButton::clicked, this, &MyPlaylistsDialog::Logout);
	connect(reloadButton, &QPushButton::clicked, this, &MyPlaylistsDialog::LoadPlaylists);
	connect(addButton, &QPushButton::clicked, this, &MyPlaylistsDialog::AddSelected);
	connect(close, &QPushButton::clicked, this, &QDialog::reject);

	UpdateStatus();
	if (loggedIn)
		LoadPlaylists();
}

MyPlaylistsDialog::~MyPlaylistsDialog()
{
	StopServer();
}

void MyPlaylistsDialog::UpdateStatus()
{
	OBSDataAutoRelease st = OAuthStatus();
	configured = st && obs_data_get_bool(st, "configured");
	loggedIn = st && obs_data_get_bool(st, "logged_in");
	QString channel = st ? QString::fromUtf8(obs_data_get_string(st, "channel")) : QString();

	if (!st)
		status->setText(T("MyPlaylists.NoModule"));
	else if (!configured)
		status->setText(T("MyPlaylists.NotConfigured"));
	else if (!loggedIn)
		status->setText(server ? T("MyPlaylists.WaitingBrowser") : T("MyPlaylists.LoggedOut"));
	else
		status->setText(T("MyPlaylists.LoggedIn").arg(channel.isEmpty() ? QString("?") : channel));

	configButton->setEnabled(st != nullptr);
	loginButton->setEnabled(configured && !loggedIn);
	logoutButton->setEnabled(loggedIn);
	reloadButton->setEnabled(loggedIn);
	addButton->setEnabled(loggedIn);
	if (!loggedIn)
		list->clear();
}

void MyPlaylistsDialog::ConfigureClient()
{
	OBSDataAutoRelease st = OAuthStatus();

	QDialog dlg(this);
	dlg.setWindowTitle(T("MyPlaylists.Configure"));
	auto *help = new QLabel(T("MyPlaylists.ConfigureHelp"), &dlg);
	help->setWordWrap(true);
	help->setTextFormat(Qt::RichText);
	help->setOpenExternalLinks(true);
	auto *id = new QLineEdit(st ? QString::fromUtf8(obs_data_get_string(st, "client_id")) : QString(), &dlg);
	auto *secret = new QLineEdit(&dlg);
	secret->setEchoMode(QLineEdit::Password);
	secret->setPlaceholderText(T("MyPlaylists.SecretPlaceholder"));
	auto *form = new QFormLayout;
	form->addRow(T("MyPlaylists.ClientId"), id);
	form->addRow(T("MyPlaylists.ClientSecret"), secret);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	auto *layout = new QVBoxLayout(&dlg);
	layout->addWidget(help);
	layout->addLayout(form);
	layout->addWidget(buttons);
	dlg.resize(520, 0);

	if (dlg.exec() != QDialog::Accepted)
		return;
	if (id->text().trimmed().isEmpty() || secret->text().trimmed().isEmpty())
		return;

	calldata_t cd = {};
	calldata_set_string(&cd, "client_id", id->text().trimmed().toUtf8().constData());
	calldata_set_string(&cd, "client_secret", secret->text().trimmed().toUtf8().constData());
	proc_handler_call(obs_get_proc_handler(), "youtube_playlist_oauth_set_client", &cd);
	calldata_free(&cd);
	UpdateStatus();
}

void MyPlaylistsDialog::Login()
{
	OBSDataAutoRelease st = OAuthStatus();
	if (!st)
		return;
	QString clientId = QString::fromUtf8(obs_data_get_string(st, "client_id"));

	StopServer();
	server = new QTcpServer(this);
	if (!server->listen(QHostAddress::LocalHost, 0)) {
		QMessageBox::warning(this, T("MyPlaylists.Title"), server->errorString());
		StopServer();
		return;
	}
	connect(server, &QTcpServer::newConnection, this, &MyPlaylistsDialog::IncomingConnection);

	verifier = RandomToken(64);
	state = RandomToken(24);
	redirectUri = QString("http://127.0.0.1:%1").arg(server->serverPort());
	QByteArray challenge = QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256)
				       .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

	QUrl url("https://accounts.google.com/o/oauth2/v2/auth");
	QUrlQuery q;
	q.addQueryItem("client_id", clientId);
	q.addQueryItem("redirect_uri", redirectUri);
	q.addQueryItem("response_type", "code");
	q.addQueryItem("scope", "https://www.googleapis.com/auth/youtube.readonly");
	q.addQueryItem("code_challenge", QString::fromLatin1(challenge));
	q.addQueryItem("code_challenge_method", "S256");
	q.addQueryItem("access_type", "offline");
	q.addQueryItem("prompt", "consent");
	q.addQueryItem("state", state);
	url.setQuery(q);

	QDesktopServices::openUrl(url);
	UpdateStatus();
}

void MyPlaylistsDialog::StopServer()
{
	if (server) {
		server->close();
		server->deleteLater();
		server = nullptr;
	}
}

void MyPlaylistsDialog::IncomingConnection()
{
	while (server && server->hasPendingConnections()) {
		QTcpSocket *sock = server->nextPendingConnection();
		connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
		connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
			QByteArray request = sock->readAll();
			QList<QByteArray> parts = request.split(' ');
			if (parts.size() < 2 || parts[0] != "GET") {
				sock->close();
				return;
			}
			QUrl url("http://127.0.0.1" + QString::fromLatin1(parts[1]));
			QUrlQuery q(url);
			if (!q.hasQueryItem("code") && !q.hasQueryItem("error")) {
				/* favicon and the like */
				sock->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
				sock->disconnectFromHost();
				return;
			}

			bool stateOk = q.queryItemValue("state") == state;
			QString code = q.queryItemValue("code");
			QString error = !stateOk ? QString("state mismatch") : q.queryItemValue("error");

			QByteArray html = QString("<!doctype html><meta charset=utf-8><title>OBS</title>"
						  "<body style='font-family:sans-serif;padding:3em'><h2>%1</h2></body>")
						  .arg(error.isEmpty() ? T("MyPlaylists.BrowserDone")
								       : T("MyPlaylists.BrowserFailed"))
						  .toUtf8();
			sock->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n");
			sock->write(QString("Content-Length: %1\r\n\r\n").arg(html.size()).toLatin1());
			sock->write(html);
			sock->disconnectFromHost();

			StopServer();
			FinishLogin(error.isEmpty() ? code : QString(), error);
		});
	}
}

void MyPlaylistsDialog::FinishLogin(const QString &code, const QString &error)
{
	raise();
	activateWindow();
	if (!error.isEmpty() || code.isEmpty()) {
		UpdateStatus();
		QMessageBox::warning(this, T("MyPlaylists.Title"), T("MyPlaylists.LoginFailed").arg(error));
		return;
	}

	status->setText(T("MyPlaylists.Exchanging"));
	QByteArray c = code.toUtf8(), v = verifier.toUtf8(), r = redirectUri.toUtf8();
	RunAsync<QString>(
		this,
		[c, v, r]() -> QString {
			calldata_t cd = {};
			calldata_set_string(&cd, "code", c.constData());
			calldata_set_string(&cd, "verifier", v.constData());
			calldata_set_string(&cd, "redirect_uri", r.constData());
			proc_handler_call(obs_get_proc_handler(), "youtube_playlist_oauth_exchange", &cd);
			QString err = calldata_bool(&cd, "ok") ? QString()
							       : QString::fromUtf8(calldata_string(&cd, "error"));
			if (err.isNull() && !calldata_bool(&cd, "ok"))
				err = "error";
			calldata_free(&cd);
			return err;
		},
		[this](const QString &err) {
			UpdateStatus();
			if (!err.isEmpty())
				QMessageBox::warning(this, T("MyPlaylists.Title"), T("MyPlaylists.LoginFailed").arg(err));
			else
				LoadPlaylists();
		});
}

void MyPlaylistsDialog::Logout()
{
	RunAsync<bool>(
		this,
		[]() {
			calldata_t cd = {};
			proc_handler_call(obs_get_proc_handler(), "youtube_playlist_oauth_logout", &cd);
			calldata_free(&cd);
			return true;
		},
		[this](const bool &) { UpdateStatus(); });
}

void MyPlaylistsDialog::LoadPlaylists()
{
	list->clear();
	reloadButton->setEnabled(false);
	status->setText(T("MyPlaylists.Loading"));

	struct Result {
		bool ok;
		QString json;
		QString error;
	};
	RunAsync<Result>(
		this,
		[]() {
			calldata_t cd = {};
			proc_handler_call(obs_get_proc_handler(), "youtube_playlist_my_playlists", &cd);
			Result r{calldata_bool(&cd, "ok"), QString::fromUtf8(calldata_string(&cd, "json")),
				 QString::fromUtf8(calldata_string(&cd, "error"))};
			calldata_free(&cd);
			return r;
		},
		[this](const Result &r) {
			UpdateStatus();
			if (!r.ok) {
				status->setText(T("MyPlaylists.LoadFailed").arg(r.error));
				return;
			}
			OBSDataAutoRelease root = obs_data_create_from_json(r.json.toUtf8().constData());
			OBSDataArrayAutoRelease arr = obs_data_get_array(root, "playlists");
			for (size_t i = 0, n = obs_data_array_count(arr); i < n; i++) {
				OBSDataAutoRelease p = obs_data_array_item(arr, i);
				auto *row = new QTreeWidgetItem(list);
				row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
				row->setCheckState(ColTitle, Qt::Unchecked);
				row->setText(ColTitle, QString::fromUtf8(obs_data_get_string(p, "title")));
				long long count = obs_data_get_int(p, "count");
				row->setText(ColCount, count >= 0 ? QString::number(count) : QString());
				row->setTextAlignment(ColCount, Qt::AlignRight | Qt::AlignVCenter);
				QString privacy = QString::fromUtf8(obs_data_get_string(p, "privacy"));
				row->setText(ColPrivacy, T(("Privacy." + privacy.toStdString()).c_str()));
				row->setData(ColTitle, Qt::UserRole, QString::fromUtf8(obs_data_get_string(p, "id")));
			}
			if (!list->topLevelItemCount())
				status->setText(status->text() + "\n" + T("MyPlaylists.Empty"));
		});
}

void MyPlaylistsDialog::AddSelected()
{
	int added = 0;
	for (int i = 0; i < list->topLevelItemCount(); i++) {
		QTreeWidgetItem *row = list->topLevelItem(i);
		if (row->checkState(ColTitle) != Qt::Checked)
			continue;
		add("https://www.youtube.com/playlist?list=" + row->data(ColTitle, Qt::UserRole).toString());
		row->setCheckState(ColTitle, Qt::Unchecked);
		added++;
	}
	if (added)
		accept();
}
