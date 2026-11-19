#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

/* Dock that shows the queue of a "youtube_playlist_source" and controls it.
 * It talks to the source only through public OBS APIs (media controls,
 * settings) and the source's procedures (get_queue, play_index,
 * refresh_lists), polling twice per second. */
class PlaylistDock : public QWidget {
	Q_OBJECT

public:
	explicit PlaylistDock(QWidget *parent = nullptr);

private slots:
	void Refresh();
	void SourceChanged(int index);
	void PlayPause();
	void Stop();
	void Next();
	void Previous();
	void AddEntry();
	void RefreshLists();
	void ItemActivated(QTreeWidgetItem *item, int column);

private:
	void UpdateSourceList();
	void UpdateQueue();
	void UpdateNowPlaying();
	QString SelectedSource() const;

	QComboBox *sourceCombo;
	QLabel *nowTitle;
	QLabel *nowTime;
	QLabel *stateLabel;
	QPushButton *prevButton;
	QPushButton *playButton;
	QPushButton *stopButton;
	QPushButton *nextButton;
	QLineEdit *entryEdit;
	QPushButton *addButton;
	QPushButton *refreshListsButton;
	QTreeWidget *queue;
	QTimer *timer;

	QStringList shownEntries; /* entries currently in the tree */
	QString currentTitle;
};
