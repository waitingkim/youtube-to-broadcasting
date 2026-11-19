#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>

class QCheckBox;
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
	void ShowContextMenu(const QPoint &pos);
	void MoveUp();
	void MoveDown();
	void RemoveSelected();
	void LoopToggled(bool checked);
	void SelectionChanged();
	void OpenMyPlaylists();
	void SetRelayMode(const char *mode);

private:
	void UpdateSourceList();
	void UpdateQueue();
	void UpdateNowPlaying();
	QString SelectedSource() const;

	/* queue editing works on entries of the source's "playlist" setting;
	 * items expanded from a playlist/channel map to that one entry */
	void MoveEntry(int settingIndex, int delta);
	void RemoveEntry(int settingIndex);
	void PlayItem(QTreeWidgetItem *item);
	bool AddEntryText(const QString &text);

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
	QPushButton *myPlaylistsButton;
	QPushButton *upButton;
	QPushButton *downButton;
	QPushButton *removeButton;
	QCheckBox *loopCheck;
	QWidget *relayRow;
	QLabel *relayLabel;
	QPushButton *relayAutoButton;
	QPushButton *relayLiveButton;
	QPushButton *relayPlaylistButton;
	QTreeWidget *queue;
	QTimer *timer;

	QStringList shownEntries; /* entries currently in the tree */
	QString selectEntry;      /* re-select this entry after the next rebuild */
	QString currentTitle;
	bool relayOnAir = false;
};
