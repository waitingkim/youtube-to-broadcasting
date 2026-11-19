#pragma once

#include <QDialog>
#include <QString>
#include <functional>

class QLabel;
class QPushButton;
class QTcpServer;
class QTreeWidget;

/* "My playlists": Google login (OAuth, loopback redirect + PKCE in the
 * system browser) and a picker for the account's playlists.  Tokens and the
 * API client live in the youtube-playlist module; this dialog only drives
 * the browser part and calls the module's global procedures. */
class MyPlaylistsDialog : public QDialog {
	Q_OBJECT

public:
	using AddFn = std::function<void(const QString &url)>;
	MyPlaylistsDialog(QWidget *parent, AddFn add);
	~MyPlaylistsDialog();

private slots:
	void ConfigureClient();
	void Login();
	void Logout();
	void LoadPlaylists();
	void AddSelected();
	void IncomingConnection();

private:
	void UpdateStatus();
	void StopServer();
	void FinishLogin(const QString &code, const QString &error);

	AddFn add;
	QLabel *status;
	QPushButton *configButton;
	QPushButton *loginButton;
	QPushButton *logoutButton;
	QPushButton *reloadButton;
	QPushButton *addButton;
	QTreeWidget *list;

	QTcpServer *server = nullptr;
	QString verifier;
	QString state;
	QString redirectUri;
	bool loggedIn = false;
	bool configured = false;
};
