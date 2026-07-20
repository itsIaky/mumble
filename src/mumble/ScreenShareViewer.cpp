// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#define GL_SILENCE_DEPRECATION

#include "ScreenShareViewer.h"

#include <QtGui/QCloseEvent>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions>
#include <QtOpenGL/QOpenGLShaderProgram>
#include <QtOpenGL/QOpenGLTexture>
#include <QtOpenGL/QOpenGLVertexArrayObject>
#include <QtOpenGL/QOpenGLBuffer>
#include <QtOpenGLWidgets/QOpenGLWidget>
#include <QtGui/QResizeEvent>
#include <QtGui/QShowEvent>
#include <QtGui/QEnterEvent>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QSlider>
#include <QtWidgets/QStyle>
#include <QtCore/QTimer>
#include <QtCore/QDateTime>

// OpenGL functions - use QOpenGLFunctions_3_3_Core for core profile
#include <QtGui/QOpenGLFunctions_3_3_Core>

// Custom QOpenGLWidget that forwards virtual functions to ScreenShareViewer
class ScreenShareGLWidget : public QOpenGLWidget {
public:
	explicit ScreenShareGLWidget(ScreenShareViewer *viewer, QWidget *parent = nullptr)
		: QOpenGLWidget(parent), m_viewer(viewer) {}

protected:
	void initializeGL() override { if (m_viewer) m_viewer->initializeGL(); }
	void resizeGL(int w, int h) override { if (m_viewer) m_viewer->resizeGL(w, h); }
	void paintGL() override { if (m_viewer) m_viewer->paintGL(); }

private:
	ScreenShareViewer *m_viewer = nullptr;
};

static const char *vertexShaderSource = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char *fragmentShaderSource = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTexture;
void main() {
    fragColor = texture(uTexture, vTexCoord);
}
)";

ScreenShareViewer::ScreenShareViewer(quint32 senderSession, const QString &senderName, QWidget *parent)
	: QDialog(parent, Qt::Window), m_senderSession(senderSession) {
	setWindowTitle(tr("%1's screen").arg(senderName));
	setAttribute(Qt::WA_DeleteOnClose, false);
	setMinimumSize(320, 240);

	setupUi();
	resize(800, 600);
}

ScreenShareViewer::~ScreenShareViewer() {
	if (m_glWidget && m_glWidget->context()) {
		m_glWidget->makeCurrent();
		if (m_texture) {
			delete m_texture;
			m_texture = nullptr;
		}
		if (m_program) {
			delete m_program;
			m_program = nullptr;
		}
		if (m_vao) {
			glDeleteVertexArrays(1, &m_vao);
			m_vao = 0;
		}
		if (m_vbo) {
			glDeleteBuffers(1, &m_vbo);
			m_vbo = 0;
		}
		m_glWidget->doneCurrent();
	}
}

void ScreenShareViewer::setupUi() {
	m_layout = new QVBoxLayout(this);
	m_layout->setContentsMargins(0, 0, 0, 0);
	m_layout->setSpacing(0);

	// Try to create OpenGL widget first
	m_glWidget = new ScreenShareGLWidget(this, this);
	m_glWidget->setMinimumSize(320, 240);
	m_glWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	m_glWidget->setFocusPolicy(Qt::NoFocus);

	// Check if OpenGL is available
	QSurfaceFormat format = m_glWidget->format();
	format.setVersion(3, 3);
	format.setProfile(QSurfaceFormat::CoreProfile);
	format.setSamples(0); // No MSAA for performance
	m_glWidget->setFormat(format);

	m_layout->addWidget(m_glWidget, 1);

	// Stats overlay (initially hidden)
	m_statsLabel = new QLabel(this);
	m_statsLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
	m_statsLabel->setStyleSheet("QLabel { color: white; background-color: rgba(0, 0, 0, 180); "
								"padding: 8px; border-radius: 4px; font-family: monospace; font-size: 11px; }");
	m_statsLabel->setVisible(false);
	m_statsLabel->raise();

	// Control bar (initially hidden, shown on hover)
	QWidget *controlBar = new QWidget(this);
	controlBar->setAttribute(Qt::WA_TranslucentBackground);
	controlBar->setFixedHeight(40);
	controlBar->setStyleSheet("QWidget { background-color: rgba(0, 0, 0, 180); border-radius: 4px; }");
	controlBar->setVisible(false);

	QHBoxLayout *controlLayout = new QHBoxLayout(controlBar);
	controlLayout->setContentsMargins(12, 4, 12, 4);
	controlLayout->setSpacing(8);

	m_fullscreenBtn = new QPushButton(tr("Fullscreen"), controlBar);
	m_fullscreenBtn->setCheckable(true);
	connect(m_fullscreenBtn, &QPushButton::toggled, this, &ScreenShareViewer::toggleFullscreen);

	m_statsCheckBox = new QCheckBox(tr("Stats"), controlBar);
	m_statsCheckBox->setChecked(false);
	connect(m_statsCheckBox, &QCheckBox::toggled, this, &ScreenShareViewer::toggleStats);

	m_qualitySlider = new QSlider(Qt::Horizontal, controlBar);
	m_qualitySlider->setRange(1, 100);
	m_qualitySlider->setValue(80);
	m_qualitySlider->setFixedWidth(120);
	m_qualitySlider->setToolTip(tr("Quality (affects decoding performance)"));
	connect(m_qualitySlider, &QSlider::valueChanged, this, &ScreenShareViewer::setQuality);

	controlLayout->addStretch();
	controlLayout->addWidget(m_fullscreenBtn);
	controlLayout->addWidget(m_statsCheckBox);
	controlLayout->addWidget(new QLabel(tr("Quality:"), controlBar));
	controlLayout->addWidget(m_qualitySlider);

	m_layout->addWidget(controlBar, 0, Qt::AlignBottom | Qt::AlignHCenter);

	// Store control bar for hover handling
	m_controlBar = controlBar;
	setMouseTracking(true);
	m_glWidget->setMouseTracking(true);

	// Timer for stats update
	m_statsTimer = new QTimer(this);
	m_statsTimer->setInterval(1000);
	connect(m_statsTimer, &QTimer::timeout, this, &ScreenShareViewer::updateStats);
	m_statsTimer->start();
}

void ScreenShareViewer::initializeGL() {
	if (m_glInitialized)
		return;

	// Initialize OpenGL functions
	initializeOpenGLFunctions();

	// Create shader program
	m_program = new QOpenGLShaderProgram();
	m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource);
	m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSource);
	if (!m_program->link()) {
		qWarning() << "Failed to link shader program:" << m_program->log();
		m_useOpenGL = false;
		fallbackToCpuRendering();
		return;
	}

	// Create VAO and VBO for full-screen quad
	glGenVertexArrays(1, &m_vao);
	glGenBuffers(1, &m_vbo);

	glBindVertexArray(m_vao);
	glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

	// Full-screen quad vertices (position + texcoord)
	GLfloat vertices[] = {
		// Positions    // TexCoords
		-1.0f,  1.0f,   0.0f, 1.0f,  // Top-left
		-1.0f, -1.0f,   0.0f, 0.0f,  // Bottom-left
		 1.0f,  1.0f,   1.0f, 1.0f,  // Top-right
		 1.0f, -1.0f,   1.0f, 0.0f   // Bottom-right
	};

	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	// Position attribute
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void *)0);
	glEnableVertexAttribArray(0);

	// TexCoord attribute
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void *)(2 * sizeof(GLfloat)));
	glEnableVertexAttribArray(1);

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// Create texture
	m_texture = new QOpenGLTexture(QOpenGLTexture::Target2D);
	m_texture->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
	m_texture->setMagnificationFilter(QOpenGLTexture::Linear);
	m_texture->setWrapMode(QOpenGLTexture::ClampToEdge);

	m_glInitialized = true;
}

void ScreenShareViewer::fallbackToCpuRendering() {
	if (m_imageLabel)
		return; // Already fallen back

	m_useOpenGL = false;

	// Remove GL widget
	m_layout->removeWidget(m_glWidget);
	m_glWidget->deleteLater();
	m_glWidget = nullptr;

	// Add CPU label
	m_imageLabel = new QLabel(this);
	m_imageLabel->setAlignment(Qt::AlignCenter);
	m_imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	m_imageLabel->setMinimumSize(320, 240);
	m_imageLabel->setText(tr("Waiting for first frame…"));
	m_imageLabel->setStyleSheet("QLabel { color: #888; font-size: 14px; }");

	m_layout->insertWidget(0, m_imageLabel, 1);

	if (m_currentFrame.isNull()) {
		m_imageLabel->setText(tr("Waiting for first frame…"));
	} else {
		updateImageDisplay();
	}
}

void ScreenShareViewer::paintGL() {
	if (!m_useOpenGL || !m_glInitialized || !m_texture || !m_program)
		return;

	if (!m_frameReady)
		return;

	// Update texture with new frame
	if (!m_pendingFrame.isNull()) {
		m_texture->destroy();
		m_texture->create();
		QImage flipped = m_pendingFrame.mirrored(false, true);
		m_texture->setData(flipped, QOpenGLTexture::GenerateMipMaps);
		m_currentFrame = m_pendingFrame;
		m_pendingFrame = QImage();
		m_frameReady = false;
		m_frameCount++;
		m_totalBytes += static_cast<size_t>(m_currentFrame.sizeInBytes());
	}

	glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	m_program->bind();
	m_texture->bind(0);
	m_program->setUniformValue("uTexture", 0);

	glBindVertexArray(m_vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glBindVertexArray(0);

	m_texture->release();
	m_program->release();
}

void ScreenShareViewer::resizeGL(int w, int h) {
	if (!m_useOpenGL)
		return;

	glViewport(0, 0, w, h);
}

void ScreenShareViewer::updateFrame(QImage frame) {
	if (frame.isNull())
		return;

	// Convert to RGBA8888 for OpenGL texture
	if (frame.format() != QImage::Format_RGBA8888) {
		frame = frame.convertToFormat(QImage::Format_RGBA8888);
	}

	if (m_useOpenGL && m_glInitialized) {
		// Store for next paintGL call (on render thread)
		m_pendingFrame = std::move(frame);
		m_frameReady = true;
		m_glWidget->update(); // Trigger repaint
	} else if (m_imageLabel) {
		// CPU path
		m_currentFrame = std::move(frame);
		updateImageDisplay();
	}
}

void ScreenShareViewer::updateImageDisplay() {
	if (m_currentFrame.isNull() || !m_imageLabel)
		return;

	QSize areaSize = m_imageLabel->size();
	if (areaSize.isEmpty())
		areaSize = size();

	QPixmap scaled = QPixmap::fromImage(m_currentFrame).scaled(areaSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
	m_imageLabel->setPixmap(scaled);
}

void ScreenShareViewer::resizeEvent(QResizeEvent *event) {
	QDialog::resizeEvent(event);

	if (m_useOpenGL && m_glWidget) {
		m_glWidget->resize(event->size());
	} else if (m_imageLabel) {
		updateImageDisplay();
	}

	// Reposition stats label
	if (m_statsLabel && m_statsLabel->isVisible()) {
		m_statsLabel->move(10, 10);
	}
}

void ScreenShareViewer::closeEvent(QCloseEvent *event) {
	m_dismissed = true;
	if (m_statsTimer) {
		m_statsTimer->stop();
	}
	QDialog::closeEvent(event);
}

void ScreenShareViewer::showEvent(QShowEvent *event) {
	QDialog::showEvent(event);
	if (m_currentFrame.isNull()) {
		if (m_useOpenGL && m_glWidget) {
			m_glWidget->update();
		} else if (m_imageLabel) {
			m_imageLabel->setText(tr("Waiting for first frame…"));
		}
	}
}

bool ScreenShareViewer::isDismissed() const {
	return m_dismissed;
}

void ScreenShareViewer::showAndRefresh() {
	m_dismissed = false;
	show();
	raise();
	activateWindow();

	if (m_useOpenGL && m_glWidget) {
		m_glWidget->update();
	} else {
		updateImageDisplay();
	}
}

void ScreenShareViewer::updateStats() {
	if (!m_showStats || !m_statsLabel)
		return;

	quint64 now = static_cast<quint64>(QDateTime::currentMSecsSinceEpoch());
	if (m_lastFrameTime > 0) {
		double elapsed = static_cast<double>(now - m_lastFrameTime) / 1000.0; // seconds
		if (elapsed > 0) {
			m_currentFps = static_cast<double>(m_frameCount) / elapsed;
		}
	}
	m_lastFrameTime = now;
	m_frameCount = 0;

	QString stats = tr("FPS: %1\nResolution: %2x%3\nTotal Data: %4 MB")
						.arg(m_currentFps, 0, 'f', 1)
						.arg(m_currentFrame.width())
						.arg(m_currentFrame.height())
						.arg(static_cast<double>(m_totalBytes) / (1024.0 * 1024.0), 0, 'f', 1);

	m_statsLabel->setText(stats);
	m_statsLabel->adjustSize();
}

void ScreenShareViewer::toggleFullscreen(bool fullscreen) {
	m_fullscreen = fullscreen;
	if (fullscreen) {
		showFullScreen();
	} else {
		showNormal();
	}
}

void ScreenShareViewer::toggleStats(bool show) {
	m_showStats = show;
	if (m_statsLabel) {
		m_statsLabel->setVisible(show);
		if (show) {
			updateStats();
		}
	}
}

void ScreenShareViewer::setQuality(int quality) {
	// Quality setting could be used to signal decoder to drop frames or reduce resolution
	// For now, just store the value
	Q_UNUSED(quality);
}

void ScreenShareViewer::enterEvent(QEnterEvent *event) {
	QDialog::enterEvent(event);
	if (m_controlBar) {
		m_controlBar->setVisible(true);
	}
}

void ScreenShareViewer::leaveEvent(QEvent *event) {
	QDialog::leaveEvent(event);
	if (m_controlBar) {
		m_controlBar->setVisible(false);
	}
}