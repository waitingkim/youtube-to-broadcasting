#include "playlist-dock.hpp"
#include "my-playlists-dialog.hpp"

#include <obs-module.h>
#include <obs.hpp>

#include <algorithm>
#include <string>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#define SOURCE_ID "youtube_playlist_source"

static inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

static QString FormatTime(long long seconds)
{
	if (seconds <= 0)
		return QString();
	long long h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
	if (h)
		return QString::asprintf("%lld:%02lld:%02lld", h, m, s);
	return QString::asprintf("%lld:%02lld", m, s);
}

static QString StatusText(const char *status, long long progress)
{
	QString st = QString::fromUtf8(status);
	if (st == "playing")
		return T("Status.Playing");
	if (st == "ready")
		return T("Status.Ready");
	if (st == "downloading")
		return T("Status.Downloading").arg(progress);
	if (st == "queued")
		return T("Status.Queued");
	if (st == "waiting")
		return T("Status.Waiting");
	if (st == "failed")
		return T("Status.Failed");
	if (st == "local")
		return T("Status.Local");
	return QString();
}

enum Column { ColIndex, ColTitle, ColDuration, ColStatus };

/* per-row data (stored on column 0) */
enum Role {
	RoleItemIndex = Qt::UserRole,
	RoleSettingIndex,
	RoleFromList,
	RoleList,
	RoleEntry,
};

PlaylistDock::PlaylistDock(QWidget *parent) : QWidget(parent)
{
	setObjectName("youtubePlaylistDock");

	sourceCombo = new QComboBox(this);
	sourceCombo->setToolTip(T("Source"));

	nowTitle = new QLabel(this);
	nowTitle->setWordWrap(true);
	QFont bold = nowTitle->font();
	bold.setBold(true);
	nowTitle->setFont(bold);
	nowTime = new QLabel(this);
	stateLabel = new QLabel(this);

	prevButton = new QPushButton(T("Previous"), this);
	playButton = new QPushButton(T("Play"), this);
	stopButton = new QPushButton(T("Stop"), this);
	nextButton = new QPushButton(T("Next"), this);

	entryEdit = new QLineEdit(this);
	entryEdit->setPlaceholderText(T("AddPlaceholder"));
	addButton = new QPushButton(T("Add"), this);
	refreshListsButton = new QPushButton(T("RefreshPlaylists"), this);
	myPlaylistsButton = new QPushButton(T("MyPlaylists.Button"), this);
	upButton = new QPushButton(T("MoveUp"), this);
	downButton = new QPushButton(T("MoveDown"), this);
	removeButton = new QPushButton(T("Remove"), this);
	loopCheck = new QCheckBox(T("Loop"), this);

	queue = new QTreeWidget(this);
	queue->setColumnCount(4);
	queue->setHeaderLabels({"#", T("Column.Title"), T("Column.Duration"), T("Column.Status")});
	queue->setRootIsDecorated(false);
	queue->setUniformRowHeights(true);
	queue->setSelectionMode(QAbstractItemView::SingleSelection);
	queue->header()->setStretchLastSection(false);
	queue->header()->setSectionResizeMode(ColIndex, QHeaderView::ResizeToContents);
	queue->header()->setSectionResizeMode(ColTitle, QHeaderView::Stretch);
	queue->header()->setSectionResizeMode(ColDuration, QHeaderView::ResizeToContents);
	queue->header()->setSectionResizeMode(ColStatus, QHeaderView::ResizeToContents);
	queue->setToolTip(T("QueueTooltip"));
	queue->setContextMenuPolicy(Qt::CustomContextMenu);
	for (int c = 0; c < 4; c++)
		queue->headerItem()->setTextAlignment(c, Qt::AlignCenter);

	auto *top = new QHBoxLayout;
	top->addWidget(new QLabel(T("Source"), this));
	top->addWidget(sourceCombo, 1);

	auto *now = new QHBoxLayout;
	now->addWidget(nowTime);
	now->addStretch(1);
	now->addWidget(stateLabel);

	auto *controls = new QHBoxLayout;
	controls->addWidget(prevButton);
	controls->addWidget(playButton);
	controls->addWidget(stopButton);
	controls->addWidget(nextButton);

	auto *edit = new QHBoxLayout;
	edit->addWidget(upButton);
	edit->addWidget(downButton);
	edit->addWidget(removeButton);
	edit->addStretch(1);
	edit->addWidget(loopCheck);

	auto *add = new QHBoxLayout;
	add->addWidget(entryEdit, 1);
	add->addWidget(addButton);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(top);
	layout->addWidget(nowTitle);
	layout->addLayout(now);
	layout->addLayout(controls);
	layout->addWidget(queue, 1);
	layout->addLayout(edit);
	layout->addLayout(add);
	auto *lists = new QHBoxLayout;
	lists->addWidget(myPlaylistsButton);
	lists->addWidget(refreshListsButton);
	layout->addLayout(lists);

	connect(sourceCombo, &QComboBox::currentIndexChanged, this, &PlaylistDock::SourceChanged);
	connect(prevButton, &QPushButton::clicked, this, &PlaylistDock::Previous);
	connect(playButton, &QPushButton::clicked, this, &PlaylistDock::PlayPause);
	connect(stopButton, &QPushButton::clicked, this, &PlaylistDock::Stop);
	connect(nextButton, &QPushButton::clicked, this, &PlaylistDock::Next);
	connect(addButton, &QPushButton::clicked, this, &PlaylistDock::AddEntry);
	connect(entryEdit, &QLineEdit::returnPressed, this, &PlaylistDock::AddEntry);
	connect(refreshListsButton, &QPushButton::clicked, this, &PlaylistDock::RefreshLists);
	connect(myPlaylistsButton, &QPushButton::clicked, this, &PlaylistDock::OpenMyPlaylists);
	connect(queue, &QTreeWidget::itemDoubleClicked, this, &PlaylistDock::ItemActivated);
	connect(queue, &QTreeWidget::customContextMenuRequested, this, &PlaylistDock::ShowContextMenu);
	connect(queue, &QTreeWidget::itemSelectionChanged, this, &PlaylistDock::SelectionChanged);
	connect(upButton, &QPushButton::clicked, this, &PlaylistDock::MoveUp);
	connect(downButton, &QPushButton::clicked, this, &PlaylistDock::MoveDown);
	connect(removeButton, &QPushButton::clicked, this, &PlaylistDock::RemoveSelected);
	connect(loopCheck, &QCheckBox::toggled, this, &PlaylistDock::LoopToggled);

	for (const QKeySequence &key : {QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)}) {
		auto *shortcut = new QShortcut(key, queue);
		shortcut->setContext(Qt::WidgetShortcut);
		connect(shortcut, &QShortcut::activated, this, &PlaylistDock::RemoveSelected);
	}

	timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, &PlaylistDock::Refresh);
	timer->start(500);
}

QString PlaylistDock::SelectedSource() const
{
	return sourceCombo->currentText();
}

static OBSSourceAutoRelease GetSource(const QString &name)
{
	if (name.isEmpty())
		return nullptr;
	return obs_get_source_by_name(name.toUtf8().constData());
}

void PlaylistDock::UpdateSourceList()
{
	QStringList names;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			if (strcmp(obs_source_get_unversioned_id(source), SOURCE_ID) == 0)
				static_cast<QStringList *>(param)->append(QString::fromUtf8(obs_source_get_name(source)));
			return true;
		},
		&names);

	QStringList current;
	for (int i = 0; i < sourceCombo->count(); i++)
		current.append(sourceCombo->itemText(i));
	if (current == names)
		return;

	QString selected = SelectedSource();
	sourceCombo->blockSignals(true);
	sourceCombo->clear();
	sourceCombo->addItems(names);
	int idx = names.indexOf(selected);
	sourceCombo->setCurrentIndex(idx >= 0 ? idx : (names.isEmpty() ? -1 : 0));
	sourceCombo->blockSignals(false);
	if (idx < 0)
		SourceChanged(sourceCombo->currentIndex());
}

void PlaylistDock::SourceChanged(int)
{
	queue->clear();
	shownEntries.clear();
	Refresh();
}

void PlaylistDock::Refresh()
{
	if (!isVisible())
		return;
	UpdateSourceList();
	UpdateQueue();
	UpdateNowPlaying();
}

void PlaylistDock::UpdateQueue()
{
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	bool enabled = source != nullptr;
	for (QWidget *w : {(QWidget *)prevButton, (QWidget *)playButton, (QWidget *)stopButton,
			   (QWidget *)nextButton, (QWidget *)entryEdit, (QWidget *)addButton,
			   (QWidget *)refreshListsButton, (QWidget *)loopCheck, (QWidget *)myPlaylistsButton})
		w->setEnabled(enabled);
	SelectionChanged();

	if (!source) {
		queue->clear();
		shownEntries.clear();
		nowTitle->setText(T("NoSource"));
		currentTitle.clear();
		return;
	}

	calldata_t cd = {};
	proc_handler_t *ph = obs_source_get_proc_handler(source);
	if (!proc_handler_call(ph, "get_queue", &cd)) {
		calldata_free(&cd);
		return;
	}
	OBSDataAutoRelease root = obs_data_create_from_json(calldata_string(&cd, "json"));
	calldata_free(&cd);
	if (!root)
		return;

	OBSDataArrayAutoRelease items = obs_data_get_array(root, "items");
	long long current = obs_data_get_int(root, "current");
	size_t count = obs_data_array_count(items);

	QStringList entries;
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease item = obs_data_array_item(items, i);
		entries.append(QString::fromUtf8(obs_data_get_string(item, "entry")));
	}

	if (entries != shownEntries) {
		QTreeWidgetItem *sel = queue->currentItem();
		QString keep = !selectEntry.isEmpty() ? selectEntry
			       : sel ? sel->data(ColIndex, RoleEntry).toString()
				     : QString();
		int keepRow = sel ? queue->indexOfTopLevelItem(sel) : -1;
		selectEntry.clear();

		queue->blockSignals(true);
		queue->clear();
		QTreeWidgetItem *toSelect = nullptr;
		for (size_t i = 0; i < count; i++) {
			auto *row = new QTreeWidgetItem(queue);
			row->setTextAlignment(ColIndex, Qt::AlignRight | Qt::AlignVCenter);
			row->setData(ColIndex, RoleEntry, entries[(int)i]);
			if (!toSelect && !keep.isEmpty() && entries[(int)i] == keep)
				toSelect = row;
		}
		if (!toSelect && keepRow >= 0 && count)
			toSelect = queue->topLevelItem(std::min(keepRow, (int)count - 1));
		if (toSelect) {
			queue->setCurrentItem(toSelect);
			queue->scrollToItem(toSelect);
		}
		queue->blockSignals(false);
		shownEntries = entries;
	}

	currentTitle.clear();
	for (size_t i = 0; i < count; i++) {
		OBSDataAutoRelease item = obs_data_array_item(items, i);
		QTreeWidgetItem *row = queue->topLevelItem((int)i);
		if (!row)
			continue;

		bool isCurrent = (long long)i == current;
		QString title = QString::fromUtf8(obs_data_get_string(item, "title"));
		bool fromList = obs_data_get_bool(item, "from_list");
		QString list = QString::fromUtf8(obs_data_get_string(item, "list"));
		row->setData(ColIndex, RoleItemIndex, (int)i);
		row->setData(ColIndex, RoleSettingIndex, (int)obs_data_get_int(item, "setting_index"));
		row->setData(ColIndex, RoleFromList, fromList);
		row->setData(ColIndex, RoleList, list);
		QString tip = entries[(int)i];
		if (fromList)
			tip += "\n" + T("FromPlaylist").arg(list);
		if (row->toolTip(ColTitle) != tip)
			row->setToolTip(ColTitle, tip);
		if (isCurrent)
			currentTitle = title;

		QString index = isCurrent ? QString::fromUtf8("▶ %1").arg(i + 1) : QString::number(i + 1);
		if (row->text(ColIndex) != index)
			row->setText(ColIndex, index);
		if (row->text(ColTitle) != title)
			row->setText(ColTitle, title);
		QString dur = FormatTime(obs_data_get_int(item, "duration"));
		if (row->text(ColDuration) != dur)
			row->setText(ColDuration, dur);
		QString status = StatusText(obs_data_get_string(item, "status"), obs_data_get_int(item, "progress"));
		if (row->text(ColStatus) != status)
			row->setText(ColStatus, status);

		QFont font = row->font(ColTitle);
		if (font.bold() != isCurrent) {
			font.setBold(isCurrent);
			for (int c = 0; c < 4; c++)
				row->setFont(c, font);
		}
	}

	QString state = QString::fromUtf8(obs_data_get_string(root, "state"));
	QString stateText = T(("State." + state.toStdString()).c_str());
	if (obs_data_get_bool(root, "lists_pending"))
		stateText = T("State.expanding");
	stateLabel->setText(stateText);
	playButton->setText(state == "playing" || state == "buffering" ? T("Pause") : T("Play"));

	bool loop = obs_data_get_bool(root, "loop");
	if (loopCheck->isChecked() != loop) {
		loopCheck->blockSignals(true);
		loopCheck->setChecked(loop);
		loopCheck->blockSignals(false);
	}
}

void PlaylistDock::UpdateNowPlaying()
{
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	if (!source)
		return;
	nowTitle->setText(currentTitle.isEmpty() ? T("NothingPlaying") : currentTitle);

	long long t = obs_source_media_get_time(source) / 1000;
	long long d = obs_source_media_get_duration(source) / 1000;
	nowTime->setText(currentTitle.isEmpty() ? QString()
						: QString("%1 / %2").arg(FormatTime(t).isEmpty() ? "0:00" : FormatTime(t),
									 FormatTime(d)));
}

void PlaylistDock::PlayPause()
{
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	if (!source)
		return;
	enum obs_media_state st = obs_source_media_get_state(source);
	bool playing = st == OBS_MEDIA_STATE_PLAYING || st == OBS_MEDIA_STATE_BUFFERING;
	obs_source_media_play_pause(source, playing);
	Refresh();
}

void PlaylistDock::Stop()
{
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	if (source)
		obs_source_media_stop(source);
	Refresh();
}

void PlaylistDock::Next()
{
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	if (source)
		obs_source_media_next(source);
	Refresh();
}

void PlaylistDock::Previous()
{
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	if (source)
		obs_source_media_previous(source);
	Refresh();
}

bool PlaylistDock::AddEntryText(const QString &value)
{
	QString text = value.trimmed();
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	if (text.isEmpty() || !source)
		return false;

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	OBSDataArrayAutoRelease array = obs_data_get_array(settings, "playlist");
	if (!array) {
		array = obs_data_array_create();
		obs_data_set_array(settings, "playlist", array);
	}
	OBSDataAutoRelease entry = obs_data_create();
	obs_data_set_string(entry, "value", text.toUtf8().constData());
	obs_data_set_bool(entry, "selected", false);
	obs_data_set_bool(entry, "hidden", false);
	obs_data_array_push_back(array, entry);

	obs_source_update(source, settings);
	return true;
}

void PlaylistDock::AddEntry()
{
	if (AddEntryText(entryEdit->text()))
		entryEdit->clear();
	Refresh();
}

void PlaylistDock::OpenMyPlaylists()
{
	MyPlaylistsDialog dialog(this, [this](const QString &url) { AddEntryText(url); });
	dialog.exec();
	Refresh();
}

void PlaylistDock::RefreshLists()
{
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	if (!source)
		return;
	calldata_t cd = {};
	proc_handler_call(obs_source_get_proc_handler(source), "refresh_lists", &cd);
	calldata_free(&cd);
}

void PlaylistDock::PlayItem(QTreeWidgetItem *item)
{
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	if (!source || !item)
		return;
	calldata_t cd = {};
	calldata_set_int(&cd, "index", item->data(ColIndex, RoleItemIndex).toInt());
	proc_handler_call(obs_source_get_proc_handler(source), "play_index", &cd);
	calldata_free(&cd);
	Refresh();
}

void PlaylistDock::ItemActivated(QTreeWidgetItem *item, int)
{
	PlayItem(item);
}

void PlaylistDock::SelectionChanged()
{
	QTreeWidgetItem *item = queue->currentItem();
	bool has = item && GetSource(SelectedSource()) != nullptr;
	int pos = has ? item->data(ColIndex, RoleSettingIndex).toInt() : -1;
	bool fromList = has && item->data(ColIndex, RoleFromList).toBool();

	upButton->setEnabled(has && pos > 0);
	downButton->setEnabled(has);
	removeButton->setEnabled(has);

	const char *suffix = fromList ? ".List" : "";
	upButton->setToolTip(T((std::string("MoveUp.Tip") + suffix).c_str()));
	downButton->setToolTip(T((std::string("MoveDown.Tip") + suffix).c_str()));
	removeButton->setToolTip(T((std::string("Remove.Tip") + suffix).c_str()));
}

void PlaylistDock::MoveEntry(int settingIndex, int delta)
{
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	if (!source || settingIndex < 0)
		return;

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	OBSDataArrayAutoRelease array = obs_data_get_array(settings, "playlist");
	int count = (int)obs_data_array_count(array);
	int target = settingIndex + delta;
	if (settingIndex >= count || target < 0 || target >= count)
		return;

	OBSDataArrayAutoRelease moved = obs_data_array_create();
	for (int i = 0; i < count; i++) {
		int from = i == target ? settingIndex : (i == settingIndex ? target : i);
		OBSDataAutoRelease entry = obs_data_array_item(array, from);
		obs_data_array_push_back(moved, entry);
	}
	obs_data_set_array(settings, "playlist", moved);
	obs_source_update(source, settings);
}

void PlaylistDock::RemoveEntry(int settingIndex)
{
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	if (!source || settingIndex < 0)
		return;

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	OBSDataArrayAutoRelease array = obs_data_get_array(settings, "playlist");
	int count = (int)obs_data_array_count(array);
	if (settingIndex >= count)
		return;

	OBSDataArrayAutoRelease kept = obs_data_array_create();
	for (int i = 0; i < count; i++) {
		if (i == settingIndex)
			continue;
		OBSDataAutoRelease entry = obs_data_array_item(array, i);
		obs_data_array_push_back(kept, entry);
	}
	obs_data_set_array(settings, "playlist", kept);
	obs_source_update(source, settings);
}

void PlaylistDock::MoveUp()
{
	QTreeWidgetItem *item = queue->currentItem();
	if (!item)
		return;
	selectEntry = item->data(ColIndex, RoleEntry).toString();
	MoveEntry(item->data(ColIndex, RoleSettingIndex).toInt(), -1);
	Refresh();
}

void PlaylistDock::MoveDown()
{
	QTreeWidgetItem *item = queue->currentItem();
	if (!item)
		return;
	selectEntry = item->data(ColIndex, RoleEntry).toString();
	MoveEntry(item->data(ColIndex, RoleSettingIndex).toInt(), +1);
	Refresh();
}

void PlaylistDock::RemoveSelected()
{
	QTreeWidgetItem *item = queue->currentItem();
	if (!item)
		return;

	if (item->data(ColIndex, RoleFromList).toBool()) {
		/* removing one video would remove the whole playlist entry */
		QString list = item->data(ColIndex, RoleList).toString();
		if (QMessageBox::question(this, T("RemoveList.Title"), T("RemoveList.Text").arg(list)) !=
		    QMessageBox::Yes)
			return;
	}
	RemoveEntry(item->data(ColIndex, RoleSettingIndex).toInt());
	Refresh();
}

void PlaylistDock::LoopToggled(bool checked)
{
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	if (!source)
		return;
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_bool(settings, "loop", checked);
	obs_source_update(source, settings); /* merges into existing settings */
}

void PlaylistDock::ShowContextMenu(const QPoint &pos)
{
	QTreeWidgetItem *item = queue->itemAt(pos);
	if (!item)
		return;
	queue->setCurrentItem(item);
	bool fromList = item->data(ColIndex, RoleFromList).toBool();
	int settingIndex = item->data(ColIndex, RoleSettingIndex).toInt();

	QMenu menu(this);
	menu.addAction(T("PlayNow"), this, [this, item]() { PlayItem(item); });
	menu.addSeparator();
	QAction *up = menu.addAction(T(fromList ? "MoveUp.List" : "MoveUp"), this, &PlaylistDock::MoveUp);
	up->setEnabled(settingIndex > 0);
	menu.addAction(T(fromList ? "MoveDown.List" : "MoveDown"), this, &PlaylistDock::MoveDown);
	menu.addAction(T(fromList ? "Remove.List" : "Remove"), this, &PlaylistDock::RemoveSelected);
	if (fromList) {
		menu.addSeparator();
		menu.addAction(T("RefreshPlaylists"), this, &PlaylistDock::RefreshLists);
	}
	menu.exec(queue->viewport()->mapToGlobal(pos));
}
