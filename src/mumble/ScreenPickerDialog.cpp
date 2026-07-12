// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifdef USE_SCREEN_SHARING

#	include "ScreenPickerDialog.h"

#	include "CaptureSourceLister.h"

#	include <QtWidgets/QDialogButtonBox>
#	include <QtWidgets/QLabel>
#	include <QtWidgets/QListWidget>
#	include <QtWidgets/QPushButton>
#	include <QtWidgets/QTabWidget>
#	include <QtWidgets/QVBoxLayout>
#	include <QtWidgets/QLineEdit>
#	include <QtWidgets/QHBoxLayout>
#	include <QtWidgets/QStyle>
#	include <QtGui/QKeyEvent>
#	include <QtCore/QTimer>

static constexpr int THUMBNAIL_SIZE = 180;
static constexpr int REFRESH_INTERVAL_MS = 2000;

ScreenPickerDialog::ScreenPickerDialog(QWidget *parent) : QDialog(parent) {
	setWindowTitle(tr("Choose what to share"));
	setMinimumSize(720, 540);
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	// Main layout
	auto *mainLayout = new QVBoxLayout(this);
	mainLayout->setContentsMargins(12, 12, 12, 12);
	mainLayout->setSpacing(8);

	// Header with hint
	auto *hint = new QLabel(tr("Select a screen or window to share, then click Share."), this);
	hint->setWordWrap(true);
	hint->setStyleSheet("QLabel { color: palette(mid); font-size: 12px; padding: 4px 0; }");
	mainLayout->addWidget(hint);

	// Tab widget for Screens / Windows
	m_tabWidget = new QTabWidget(this);
	m_tabWidget->setDocumentMode(true);
	mainLayout->addWidget(m_tabWidget, 1);

	// Screens tab
	m_screensWidget = new QWidget(this);
	auto *screensLayout = new QVBoxLayout(m_screensWidget);
	screensLayout->setContentsMargins(8, 8, 8, 8);
	screensLayout->setSpacing(8);

	m_screensList = new QListWidget(m_screensWidget);
	m_screensList->setViewMode(QListView::IconMode);
	m_screensList->setIconSize(QSize(THUMBNAIL_SIZE, THUMBNAIL_SIZE * 9 / 16));
	m_screensList->setResizeMode(QListView::Adjust);
	m_screensList->setSpacing(12);
	m_screensList->setWordWrap(true);
	m_screensList->setMovement(QListView::Static);
	m_screensList->setSelectionMode(QAbstractItemView::SingleSelection);
	m_screensList->setUniformItemSizes(true);
	connect(m_screensList, &QListWidget::itemDoubleClicked, this, &ScreenPickerDialog::onItemDoubleClicked);
	connect(m_screensList, &QListWidget::itemSelectionChanged, this, &ScreenPickerDialog::onSelectionChanged);
	screensLayout->addWidget(m_screensList, 1);

	m_tabWidget->addTab(m_screensWidget, tr("Screens"));

	// Windows tab
	m_windowsWidget = new QWidget(this);
	auto *windowsLayout = new QVBoxLayout(m_windowsWidget);
	windowsLayout->setContentsMargins(8, 8, 8, 8);
	windowsLayout->setSpacing(8);

	// Search/filter bar for windows
	auto *filterLayout = new QHBoxLayout();
	filterLayout->setSpacing(8);
	auto *filterIcon = new QLabel(this);
	filterIcon->setPixmap(style()->standardIcon(QStyle::SP_FileDialogContentsView).pixmap(16, 16));
	filterLayout->addWidget(filterIcon);

	m_windowFilter = new QLineEdit(m_windowsWidget);
	m_windowFilter->setPlaceholderText(tr("Filter windows..."));
	m_windowFilter->setClearButtonEnabled(true);
	connect(m_windowFilter, &QLineEdit::textChanged, this, &ScreenPickerDialog::onFilterChanged);
	filterLayout->addWidget(m_windowFilter, 1);

	// Refresh button
	m_refreshButton = new QPushButton(tr("Refresh"), m_windowsWidget);
	m_refreshButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
	connect(m_refreshButton, &QPushButton::clicked, this, &ScreenPickerDialog::refreshSources);
	filterLayout->addWidget(m_refreshButton);

	windowsLayout->addLayout(filterLayout);

	m_windowsList = new QListWidget(m_windowsWidget);
	m_windowsList->setViewMode(QListView::IconMode);
	m_windowsList->setIconSize(QSize(THUMBNAIL_SIZE, THUMBNAIL_SIZE * 9 / 16));
	m_windowsList->setResizeMode(QListView::Adjust);
	m_windowsList->setSpacing(12);
	m_windowsList->setWordWrap(true);
	m_windowsList->setMovement(QListView::Static);
	m_windowsList->setSelectionMode(QAbstractItemView::SingleSelection);
	m_windowsList->setUniformItemSizes(true);
	connect(m_windowsList, &QListWidget::itemDoubleClicked, this, &ScreenPickerDialog::onItemDoubleClicked);
	connect(m_windowsList, &QListWidget::itemSelectionChanged, this, &ScreenPickerDialog::onSelectionChanged);
	windowsLayout->addWidget(m_windowsList, 1);

	m_tabWidget->addTab(m_windowsWidget, tr("Windows"));

	// Button box
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	m_shareButton = buttons->button(QDialogButtonBox::Ok);
	m_shareButton->setText(tr("Share"));
	m_shareButton->setEnabled(false);
	m_shareButton->setDefault(true);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	mainLayout->addWidget(buttons);

	// Status label
	m_statusLabel = new QLabel(this);
	m_statusLabel->setStyleSheet("QLabel { color: palette(mid); font-size: 11px; padding: 4px 0; }");
	mainLayout->addWidget(m_statusLabel);

	// Initial population
	refreshSources();

	// Auto-refresh timer for live thumbnail updates
	m_refreshTimer = new QTimer(this);
	m_refreshTimer->setInterval(REFRESH_INTERVAL_MS);
	m_refreshTimer->setSingleShot(false);
	connect(m_refreshTimer, &QTimer::timeout, this, &ScreenPickerDialog::updateThumbnails);
	m_refreshTimer->start();

	// Focus the filter when windows tab is selected
	connect(m_tabWidget, &QTabWidget::currentChanged, this, [this](int index) {
		if (index == 1) { // Windows tab
			m_windowFilter->setFocus();
		}
	});
}

ScreenPickerDialog::~ScreenPickerDialog() {
	if (m_refreshTimer) {
		m_refreshTimer->stop();
	}
}

void ScreenPickerDialog::refreshSources() {
	// Store current selections
	CaptureSource prevScreenSource = selectedSource();
	CaptureSource prevWindowSource = selectedSource();

	// Get fresh sources
	m_screenSources.clear();
	m_windowSources.clear();
	QList<CaptureSource> sources = listCaptureSources();

	// Clear and repopulate screens
	m_screensList->clear();
	for (const CaptureSource &src : sources) {
		if (src.type == CaptureSource::Type::EntireScreen) {
			m_screenSources.append(src);
			QIcon icon = src.thumbnail.isNull() ? QIcon() : QIcon(src.thumbnail);
			auto *item = new QListWidgetItem(icon, src.displayName);
			item->setToolTip(src.displayName);
			item->setData(Qt::UserRole, m_screenSources.size() - 1);
			m_screensList->addItem(item);
		}
	}

	// Clear and repopulate windows
	m_windowsList->clear();
	for (const CaptureSource &src : sources) {
		if (src.type == CaptureSource::Type::Window) {
			m_windowSources.append(src);
			QIcon icon = src.thumbnail.isNull() ? QIcon() : QIcon(src.thumbnail);
			auto *item = new QListWidgetItem(icon, src.displayName);
			item->setToolTip(src.displayName);
			item->setData(Qt::UserRole, m_windowSources.size() - 1);
			m_windowsList->addItem(item);
		}
	}

	// Restore selections if possible
	restoreSelection(prevScreenSource, m_screensList, m_screenSources);
	restoreSelection(prevWindowSource, m_windowsList, m_windowSources);

	// Apply current filter
	onFilterChanged(m_windowFilter->text());

	updateStatusLabel();
}

void ScreenPickerDialog::updateThumbnails() {
	// Update thumbnails for visible items only (performance)
	auto updateListThumbnails = [](QListWidget *list, const QList<CaptureSource> &sources) {
		for (int i = 0; i < list->count(); ++i) {
			QListWidgetItem *item = list->item(i);
			if (!item || item->isHidden())
				continue;

			int sourceIdx = item->data(Qt::UserRole).toInt();
			if (sourceIdx < 0 || sourceIdx >= sources.size())
				continue;

			const CaptureSource &src = sources[sourceIdx];
			QImage img = grabCaptureSource(src);
			if (!img.isNull()) {
				QPixmap freshThumb = QPixmap::fromImage(img).scaled(
					THUMBNAIL_SIZE, THUMBNAIL_SIZE * 9 / 16, Qt::KeepAspectRatio, Qt::SmoothTransformation);
				if (!freshThumb.isNull()) {
					item->setIcon(QIcon(freshThumb));
				}
			}
		}
	};

	updateListThumbnails(m_screensList, m_screenSources);
	updateListThumbnails(m_windowsList, m_windowSources);
}

void ScreenPickerDialog::onFilterChanged(const QString &text) {
	QString filter = text.trimmed().toLower();
	for (int i = 0; i < m_windowsList->count(); ++i) {
		QListWidgetItem *item = m_windowsList->item(i);
		if (!item)
			continue;

		bool match = filter.isEmpty() || item->text().toLower().contains(filter);
		item->setHidden(!match);
	}
	updateStatusLabel();
}

void ScreenPickerDialog::onSelectionChanged() {
	m_shareButton->setEnabled(hasValidSelection());
	updateStatusLabel();
}

void ScreenPickerDialog::onItemDoubleClicked(QListWidgetItem *) {
	if (hasValidSelection()) {
		accept();
	}
}

bool ScreenPickerDialog::hasValidSelection() const {
	QListWidget *currentList = (m_tabWidget->currentIndex() == 0) ? m_screensList : m_windowsList;
	return currentList && currentList->currentRow() >= 0 && !currentList->currentItem()->isHidden();
}

CaptureSource ScreenPickerDialog::selectedSource() const {
	QListWidget *currentList = (m_tabWidget->currentIndex() == 0) ? m_screensList : m_windowsList;
	const QList<CaptureSource> &currentSources = (m_tabWidget->currentIndex() == 0) ? m_screenSources : m_windowSources;

	if (!currentList)
		return {};

	QListWidgetItem *item = currentList->currentItem();
	if (!item || item->isHidden())
		return {};

	int sourceIdx = item->data(Qt::UserRole).toInt();
	if (sourceIdx >= 0 && sourceIdx < currentSources.size()) {
		return currentSources[sourceIdx];
	}
	return {};
}

void ScreenPickerDialog::restoreSelection(const CaptureSource &prevSource, QListWidget *list,
										  const QList<CaptureSource> &sources) {
	if (prevSource.type == CaptureSource::Type::EntireScreen) {
		for (int i = 0; i < sources.size(); ++i) {
			if (sources[i].screenIndex == prevSource.screenIndex) {
				list->setCurrentRow(i);
				break;
			}
		}
	} else if (prevSource.type == CaptureSource::Type::Window) {
		for (int i = 0; i < sources.size(); ++i) {
			if (sources[i].nativeWindowId == prevSource.nativeWindowId) {
				list->setCurrentRow(i);
				break;
			}
		}
	}
}

void ScreenPickerDialog::updateStatusLabel() {
	QListWidget *currentList = (m_tabWidget->currentIndex() == 0) ? m_screensList : m_windowsList;
	int visibleCount = 0;
	for (int i = 0; i < currentList->count(); ++i) {
		if (!currentList->item(i)->isHidden())
			++visibleCount;
	}

	QString tabName = (m_tabWidget->currentIndex() == 0) ? tr("screen") : tr("window");
	m_statusLabel->setText(tr("%1 %2(s) available").arg(visibleCount).arg(tabName));
}

void ScreenPickerDialog::keyPressEvent(QKeyEvent *event) {
	if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
		if (hasValidSelection()) {
			accept();
			return;
		}
	} else if (event->key() == Qt::Key_Escape) {
		reject();
		return;
	} else if (event->key() == Qt::Key_F5) {
		refreshSources();
		return;
	}
	QDialog::keyPressEvent(event);
}

void ScreenPickerDialog::closeEvent(QCloseEvent *event) {
	if (m_refreshTimer) {
		m_refreshTimer->stop();
	}
	QDialog::closeEvent(event);
}

#endif // USE_SCREEN_SHARING