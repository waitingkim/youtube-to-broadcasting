#include "playlist-dock.hpp"

#include <obs-module.h>
#include <obs.hpp>

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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

	auto *add = new QHBoxLayout;
	add->addWidget(entryEdit, 1);
	add->addWidget(addButton);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(top);
	layout->addWidget(nowTitle);
	layout->addLayout(now);
	layout->addLayout(controls);
	layout->addWidget(queue, 1);
	layout->addLayout(add);
	layout->addWidget(refreshListsButton);

	connect(sourceCombo, &QComboBox::currentIndexChanged, this, &PlaylistDock::SourceChanged);
	connect(prevButton, &QPushButton::clicked, this, &PlaylistDock::Previous);
	connect(playButton, &QPushButton::clicked, this, &PlaylistDock::PlayPause);
	connect(stopButton, &QPushButton::clicked, this, &PlaylistDock::Stop);
	connect(nextButton, &QPushButton::clicked, this, &PlaylistDock::Next);
	connect(addButton, &QPushButton::clicked, this, &PlaylistDock::AddEntry);
	connect(entryEdit, &QLineEdit::returnPressed, this, &PlaylistDock::AddEntry);
	connect(refreshListsButton, &QPushButton::clicked, this, &PlaylistDock::RefreshLists);
	connect(queue, &QTreeWidget::itemDoubleClicked, this, &PlaylistDock::ItemActivated);

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
			   (QWidget *)refreshListsButton})
		w->setEnabled(enabled);

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
		queue->clear();
		for (size_t i = 0; i < count; i++) {
			auto *row = new QTreeWidgetItem(queue);
			row->setData(ColIndex, Qt::UserRole, (int)i);
			row->setTextAlignment(ColIndex, Qt::AlignRight | Qt::AlignVCenter);
			row->setToolTip(ColTitle, entries[(int)i]);
		}
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

void PlaylistDock::AddEntry()
{
	QString text = entryEdit->text().trimmed();
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	if (text.isEmpty() || !source)
		return;

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
	entryEdit->clear();
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

void PlaylistDock::ItemActivated(QTreeWidgetItem *item, int)
{
	OBSSourceAutoRelease source = GetSource(SelectedSource());
	if (!source || !item)
		return;
	calldata_t cd = {};
	calldata_set_int(&cd, "index", item->data(ColIndex, Qt::UserRole).toInt());
	proc_handler_call(obs_source_get_proc_handler(source), "play_index", &cd);
	calldata_free(&cd);
	Refresh();
}
