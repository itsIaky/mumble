// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENPICKERDIALOG_H_
#define MUMBLE_MUMBLE_SCREENPICKERDIALOG_H_

#ifdef USE_SCREEN_SHARING

#	include "CaptureSource.h"

#	include <QtCore/QList>
#	include <QtWidgets/QDialog>

class QTabWidget;
class QListWidget;
class QListWidgetItem;
class QWidget;
class QLineEdit;
class QLabel;
class QPushButton;
class QTimer;

/// Modal dialog that lists available screens and windows for screen sharing.
/// Call exec(); if accepted, use selectedSource() to retrieve the chosen source.
class ScreenPickerDialog : public QDialog {
	Q_OBJECT
	Q_DISABLE_COPY(ScreenPickerDialog)

public:
	explicit ScreenPickerDialog(QWidget *parent = nullptr);
	~ScreenPickerDialog() override;

	/// Returns the source selected by the user, or a default-constructed CaptureSource
	/// (EntireScreen, screenIndex 0) if nothing was explicitly selected.
	CaptureSource selectedSource() const;

protected:
	void keyPressEvent(QKeyEvent *event) override;
	void closeEvent(QCloseEvent *event) override;

private slots:
	void onItemDoubleClicked(QListWidgetItem *item);
	void onFilterChanged(const QString &text);
	void onSelectionChanged();
	void refreshSources();
	void updateThumbnails();

private:
	void setupUi();
	void populateList(int tabIndex);
	void updateStatusLabel();
	bool hasValidSelection() const;
	void restoreSelection(const CaptureSource &prevSource, QListWidget *list, const QList<CaptureSource> &sources);

	QTabWidget *m_tabWidget;
	QWidget *m_screensWidget;
	QWidget *m_windowsWidget;
	QListWidget *m_screensList;
	QListWidget *m_windowsList;
	QLineEdit *m_windowFilter;
	QLabel *m_statusLabel;
	QPushButton *m_refreshButton;
	QPushButton *m_shareButton;

	QList< CaptureSource > m_sources;
	QList< CaptureSource > m_screenSources;
	QList< CaptureSource > m_windowSources;

	QTimer *m_refreshTimer;
};

#endif // USE_SCREEN_SHARING
#endif // MUMBLE_MUMBLE_SCREENPICKERDIALOG_H_