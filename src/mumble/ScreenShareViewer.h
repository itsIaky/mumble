// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#ifndef MUMBLE_MUMBLE_SCREENSHAREVIEWER_H_
#define MUMBLE_MUMBLE_SCREENSHAREVIEWER_H_

#include <QtGui/QImage>
#include <QtGui/QOpenGLFunctions_3_3_Core>
#include <QtWidgets/QDialog>

// OpenGL types
#include <QtGui/qopengl.h>

class QOpenGLWidget;
class QOpenGLTexture;
class QOpenGLShaderProgram;
class QLabel;
class QVBoxLayout;
class QPushButton;
class QCheckBox;
class QSlider;
class QWidget;
class QTimer;

/// Floating window that displays the screen share stream from a single remote user.
/// Uses OpenGL for hardware-accelerated rendering when available.
class ScreenShareViewer : public QDialog, protected QOpenGLFunctions_3_3_Core {
private:
	Q_OBJECT
	Q_DISABLE_COPY(ScreenShareViewer)

public:
	explicit ScreenShareViewer(quint32 senderSession, const QString &senderName, QWidget *parent = nullptr);
	~ScreenShareViewer() override;

	/// Returns true once the user has explicitly closed the window.
	/// While dismissed, new frames update the stored image but do not reopen the window.
	bool isDismissed() const;
	/// Show the window and repaint with the last stored frame.
	void showAndRefresh();

public slots:
	void updateFrame(QImage frame);

protected:
	void resizeEvent(QResizeEvent *event) override;
	void closeEvent(QCloseEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void enterEvent(QEnterEvent *event) override;
	void leaveEvent(QEvent *event) override;

private:
	friend class ScreenShareGLWidget;

	void setupUi();
	void updateImageDisplay();
	void updateStats();
	void toggleFullscreen(bool fullscreen);
	void toggleStats(bool show);
	void setQuality(int quality);

	// OpenGL rendering
	void initializeGL();
	void paintGL();
	void resizeGL(int w, int h);
	void fallbackToCpuRendering();

	QOpenGLWidget *m_glWidget = nullptr;
	QOpenGLTexture *m_texture = nullptr;
	QOpenGLShaderProgram *m_program = nullptr;
	GLuint m_vao = 0;
	GLuint m_vbo = 0;
	bool m_glInitialized = false;

	// Fallback CPU rendering
	QLabel *m_imageLabel = nullptr;
	QVBoxLayout *m_layout = nullptr;
	bool m_useOpenGL = true;

	// Stats overlay
	QLabel *m_statsLabel = nullptr;
	QPushButton *m_fullscreenBtn = nullptr;
	QCheckBox *m_statsCheckBox = nullptr;
	QSlider *m_qualitySlider = nullptr;
	bool m_showStats = false;
	bool m_fullscreen = false;

	QWidget *m_controlBar = nullptr;

	quint32 m_senderSession;
	QImage m_currentFrame;
	QImage m_pendingFrame;
	bool m_frameReady = false;
	bool m_dismissed = false;

	// Statistics
	quint64 m_frameCount = 0;
	quint64 m_lastFrameTime = 0;
	double m_currentFps = 0.0;
	size_t m_totalBytes = 0;
	QTimer *m_statsTimer = nullptr;
};

#endif // MUMBLE_MUMBLE_SCREENSHAREVIEWER_H_