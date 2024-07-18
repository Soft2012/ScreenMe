#include "include/screenshotdisplay.h"
#include "include/config_manager.h"
#include "include/utils.h"
#include <QApplication>
#include <QFileDialog>
#include <QClipboard>
#include <QPainter>
#include <QMouseEvent>
#include <QShortcut>
#include <QToolTip>
#include <QCursor>
#include <QWheelEvent>

ScreenshotDisplay::ScreenshotDisplay(const QPixmap& pixmap, QWidget* parent, ConfigManager* configManager)
    : QWidget(parent), originalPixmap(pixmap), selectionStarted(false), movingSelection(false), currentHandle(None), configManager(configManager),
    drawing(false), shapeDrawing(false), currentColor(Qt::black), currentTool(Editor::None), borderWidth(5),
    drawingPixmap(pixmap.size()), currentFont("Arial", 16), text("Editable Text"), textEdit(nullptr) {

    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setWindowTitle("ScreenMe");
    setWindowIcon(QIcon("resources/icon.png"));
    setAttribute(Qt::WA_QuitOnClose, false);
    setGeometry(QApplication::primaryScreen()->geometry());

    initializeEditor();
    configureShortcuts();

    drawingPixmap.fill(Qt::transparent);
    QFontMetrics fm(currentFont);
    textBoundingRect = QRect(QPoint(100, 100), fm.size(0, text));
    showFullScreen();
}

void ScreenshotDisplay::initializeEditor() {
    editor.reset(new Editor(this));
    connect(editor.get(), &Editor::toolChanged, this, &ScreenshotDisplay::onToolSelected);
    connect(editor.get(), &Editor::colorChanged, this, [this](const QColor& color) {
        update();
    });
    connect(editor.get(), &Editor::saveRequested, this, &ScreenshotDisplay::onSaveRequested);
    connect(editor.get(), &Editor::copyRequested, this, &ScreenshotDisplay::copySelectionToClipboard);
    connect(editor.get(), &Editor::publishRequested, this, &ScreenshotDisplay::onPublishRequested);
    connect(editor.get(), &Editor::closeRequested, this, &ScreenshotDisplay::onCloseRequested);
}


void ScreenshotDisplay::configureShortcuts() {
    QShortcut* escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escapeShortcut, &QShortcut::activated, [this]() {
        if (editor->getCurrentTool() != Editor::None) {
            editor->deselectTools();
            setCursor(Qt::ArrowCursor);
        }
        else {
            close();
        }
    });

    QShortcut* undoShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z), this);
    connect(undoShortcut, &QShortcut::activated, this, &ScreenshotDisplay::undo);

    QShortcut* copyShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_C), this);
    connect(copyShortcut, &QShortcut::activated, this, &ScreenshotDisplay::copySelectionToClipboard);
}

void ScreenshotDisplay::closeEvent(QCloseEvent* event) {
    emit screenshotClosed();
    if (editor) {
        editor->hide();
    }
    if (textEdit) {
        textEdit->deleteLater();
        textEdit = nullptr;
    }
    QWidget::closeEvent(event);
}

void ScreenshotDisplay::mousePressEvent(QMouseEvent* event) {
    if (editor->getCurrentTool() == Editor::None) {
        HandlePosition handle = handleAtPoint(event->pos());
        if (handle != None) {
            currentHandle = handle;
            handleOffset = event->pos() - selectionRect.topLeft();
        }
        else if (selectionRect.contains(event->pos())) {
            movingSelection = true;
            selectionOffset = event->pos() - selectionRect.topLeft();
        }
        else {
            selectionStarted = true;
            origin = event->pos();
            selectionRect = QRect(origin, QSize());
            currentHandle = None;
            movingSelection = false;
        }
    }
    else if (editor->getCurrentTool() == Editor::Text) {
        if (!textEdit) {
            textEdit = new QTextEdit(this);
            textEdit->setFont(currentFont);
            textEdit->setTextColor(currentColor);
            textEdit->setStyleSheet("background: transparent;");
            textEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            textEdit->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
            textEdit->move(event->pos());
            textEdit->show();
            textEdit->setFocus();
            textEditPosition = event->pos();
            connect(textEdit, &QTextEdit::textChanged, this, &ScreenshotDisplay::adjustTextEditSize);
        }
        else {
            finalizeTextEdit();
        }
    }
    else {
        saveStateForUndo();
        drawing = true;
        lastPoint = event->pos();
        origin = event->pos();
        if (editor->getCurrentTool() != Editor::Pen) {
            shapeDrawing = true;
            currentShapeRect = QRect(lastPoint, QSize());
        }
    }
}

void ScreenshotDisplay::mouseMoveEvent(QMouseEvent* event) {
    if (selectionRect.isValid() && editor->isHidden()) {
        updateEditorPosition();
        editor->show();
    }
    if (selectionRect.isValid()) {
        update();
    }
    if (selectionStarted) {
        selectionRect = QRect(origin, event->pos()).normalized();
        update();
        updateTooltip();
        updateEditorPosition();
    }
    else if (drawing && editor->getCurrentTool() == Editor::Pen) {
        QPixmap tempPixmap = drawingPixmap.copy();
        QPainter painter(&tempPixmap);
        painter.setPen(QPen(editor->getCurrentColor(), borderWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(lastPoint, event->pos());
        lastPoint = event->pos();
        drawingPixmap = tempPixmap;
        update();
    }
    else if (shapeDrawing) {
        currentShapeRect = QRect(lastPoint, event->pos()).normalized();
        drawingEnd = event->pos();
        update();
    }
    else if (movingSelection) {
        selectionRect.moveTopLeft(event->pos() - selectionOffset);
        update();
        updateTooltip();
        updateEditorPosition();
    }

    HandlePosition handle = handleAtPoint(event->pos());
    setCursor(cursorForHandle(handle));
}

void ScreenshotDisplay::mouseReleaseEvent(QMouseEvent* event) {
    selectionStarted = false;
    movingSelection = false;
    currentHandle = None;
    drawing = false;

    if (shapeDrawing) {
        saveStateForUndo();
        QPixmap tempPixmap = drawingPixmap.copy();
        QPainter painter(&tempPixmap);
        painter.setPen(QPen(editor->getCurrentColor(), borderWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

        switch (editor->getCurrentTool()) {
        case Editor::Rectangle:
            painter.drawRect(currentShapeRect);
            break;
        case Editor::Ellipse:
            painter.drawEllipse(currentShapeRect);
            break;
        case Editor::Line:
            painter.drawLine(lastPoint, drawingEnd);
            break;
        case Editor::Arrow:
            drawArrow(painter, lastPoint, drawingEnd);
            break;
        default:
            break;
        }

        drawingPixmap = tempPixmap;
        shapeDrawing = false;
        update();
    }

    updateTooltip();
}

void ScreenshotDisplay::keyPressEvent(QKeyEvent* event) {
    if (editor->getCurrentTool() != Editor::None && event->key() == Qt::Key_Escape) {
        if (editor->getCurrentTool() == Editor::Text && textEdit) {
            finalizeTextEdit();
        }
        else {
            editor->deselectTools();
            setCursor(Qt::ArrowCursor);
        }
    }
    else if (event->key() == Qt::Key_Escape) {
        close();
    }
    else if (event->key() == Qt::Key_C && event->modifiers() == Qt::ControlModifier) {
        copySelectionToClipboard();
        close();
    }
}

void ScreenshotDisplay::wheelEvent(QWheelEvent* event) {
    if (editor->getCurrentTool() != Editor::None && editor->getCurrentTool() != Editor::Text) {
        borderWidth += event->angleDelta().y() / 120;
        borderWidth = std::clamp(borderWidth, 1, 20);
        update();
    }
    if (editor->getCurrentTool() == Editor::Text && textEdit) {
        int delta = event->angleDelta().y() / 120;
        int newSize = currentFont.pointSize() + delta;
        if (newSize > 0) {
            currentFont.setPointSize(newSize);
            textEdit->setFont(currentFont);
            adjustTextEditSize();
            update();
        }
    }
}

void ScreenshotDisplay::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.drawPixmap(0, 0, originalPixmap);
    painter.drawPixmap(0, 0, drawingPixmap);

    if (selectionRect.isValid()) {
        painter.setPen(QPen(Qt::red, 2, Qt::DashLine));
        painter.drawRect(selectionRect);
        drawHandles(painter);
    }

    if (shapeDrawing) {
        painter.setPen(QPen(editor->getCurrentColor(), borderWidth, Qt::SolidLine));
        switch (editor->getCurrentTool()) {
        case Editor::Pen:
            painter.drawPath(drawingPath);
            break;
        case Editor::Rectangle:
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(currentShapeRect);
            break;
        case Editor::Ellipse:
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(currentShapeRect);
            break;
        case Editor::Line:
            painter.drawLine(origin, drawingEnd);
            break;
        case Editor::Arrow:
            drawArrow(painter, lastPoint, drawingEnd);
            break;
        case Editor::Text:
            painter.setFont(currentFont);
            painter.setPen(QPen(editor->getCurrentColor()));
            painter.drawText(textBoundingRect, Qt::AlignLeft, text);
            break;
        default:
            break;
        }
    }

    if (editor->getCurrentTool() != Editor::None) {
        drawBorderCircle(painter, mapFromGlobal(QCursor::pos()));
        painter.setBrush(Qt::transparent);
        painter.drawEllipse(cursorPosition, borderWidth / 2, borderWidth / 2);
    }
}

void ScreenshotDisplay::onSaveRequested() {
    QJsonObject config = configManager->loadConfig();
    QString defaultSaveFolder = config["default_save_folder"].toString();
    QString fileExtension = config["file_extension"].toString();
    QString defaultFileName = getUniqueFilePath(defaultSaveFolder, "screenshot", fileExtension);

    QString fileFilter = "PNG Files (*.png);;JPEG Files (*.jpg *.jpeg);;";
    if (fileExtension == "png") {
        fileFilter = "PNG Files (*.png);;";
    }
    else if (fileExtension == "jpg" || fileExtension == "jpeg") {
        fileFilter = "JPEG Files (*.jpg *.jpeg);;";
    }

    QString filePath = QFileDialog::getSaveFileName(this, "Save As", defaultFileName, fileFilter);

    if (!filePath.isEmpty()) {
        originalPixmap.save(filePath);
        close();
    }
}

void ScreenshotDisplay::onPublishRequested() {
    // TODO
}

void ScreenshotDisplay::onCloseRequested() {
    close();
}

void ScreenshotDisplay::copySelectionToClipboard() {
    QPixmap resultPixmap = originalPixmap;
    QPainter painter(&resultPixmap);
    painter.drawPixmap(0, 0, drawingPixmap);

    if (selectionRect.isValid()) {
        QPixmap selectedPixmap = resultPixmap.copy(selectionRect);
        QApplication::clipboard()->setPixmap(selectedPixmap);
    }
    else {
        QApplication::clipboard()->setPixmap(resultPixmap);
    }
    close();
}

void ScreenshotDisplay::updateTooltip() {
    if (selectionRect.isValid()) {
        QString tooltipText = QString("Size: %1 x %2").arg(selectionRect.width()).arg(selectionRect.height());
        QPoint tooltipPosition = selectionRect.topRight() + QPoint(10, -20);
        QToolTip::showText(mapToGlobal(tooltipPosition), tooltipText, this);
    }
}

void ScreenshotDisplay::drawHandles(QPainter& painter) {
    const int handleSize = 3;
    const QVector<QPoint> handlePoints = {
        selectionRect.topLeft(),
        selectionRect.topRight(),
        selectionRect.bottomLeft(),
        selectionRect.bottomRight(),
        selectionRect.topLeft() + QPoint(selectionRect.width() / 2, 0),
        selectionRect.bottomLeft() + QPoint(selectionRect.width() / 2, 0),
        selectionRect.topLeft() + QPoint(0, selectionRect.height() / 2),
        selectionRect.topRight() + QPoint(0, selectionRect.height() / 2)
    };
    painter.setBrush(Qt::red);
    for (const QPoint& point : handlePoints) {
        painter.drawRect(QRect(point - QPoint(handleSize / 2, handleSize / 2), QSize(handleSize * 2, handleSize * 2)));
    }
}

ScreenshotDisplay::HandlePosition ScreenshotDisplay::handleAtPoint(const QPoint& point) {
    const int handleSize = 20;
    const QRect handleRect(QPoint(0, 0), QSize(handleSize, handleSize));
    if (handleRect.translated(selectionRect.topLeft()).contains(point)) return TopLeft;
    if (handleRect.translated(selectionRect.topRight()).contains(point)) return TopRight;
    if (handleRect.translated(selectionRect.bottomLeft()).contains(point)) return BottomLeft;
    if (handleRect.translated(selectionRect.bottomRight()).contains(point)) return BottomRight;
    if (handleRect.translated(selectionRect.topLeft() + QPoint(selectionRect.width() / 2, 0)).contains(point)) return Top;
    if (handleRect.translated(selectionRect.bottomLeft() + QPoint(selectionRect.width() / 2, 0)).contains(point)) return Bottom;
    if (handleRect.translated(selectionRect.topLeft() + QPoint(0, selectionRect.height() / 2)).contains(point)) return Left;
    if (handleRect.translated(selectionRect.topRight() + QPoint(0, selectionRect.height() / 2)).contains(point)) return Right;
    return None;
}

void ScreenshotDisplay::resizeSelection(const QPoint& point) {
    switch (currentHandle) {
    case TopLeft:
        selectionRect.setTopLeft(point);
        break;
    case TopRight:
        selectionRect.setTopRight(point);
        break;
    case BottomLeft:
        selectionRect.setBottomLeft(point);
        break;
    case BottomRight:
        selectionRect.setBottomRight(point);
        break;
    case Top:
        selectionRect.setTop(point.y());
        break;
    case Bottom:
        selectionRect.setBottom(point.y());
        break;
    case Left:
        selectionRect.setLeft(point.x());
        break;
    case Right:
        selectionRect.setRight(point.x());
        break;
    default:
        break;
    }
    selectionRect = selectionRect.normalized();
}

Qt::CursorShape ScreenshotDisplay::cursorForHandle(HandlePosition handle) {
    switch (handle) {
    case TopLeft:
    case BottomRight:
        return Qt::SizeFDiagCursor;
    case TopRight:
    case BottomLeft:
        return Qt::SizeBDiagCursor;
    case Top:
    case Bottom:
        return Qt::SizeVerCursor;
    case Left:
    case Right:
        return Qt::SizeHorCursor;
    default:
        return Qt::ArrowCursor;
    }
}

void ScreenshotDisplay::onToolSelected(Editor::Tool tool) {
    currentTool = tool;
    setCursor(tool == Editor::None ? Qt::ArrowCursor : Qt::CrossCursor);
}

void ScreenshotDisplay::updateEditorPosition() {
    if (selectionRect.isValid()) {
        const int margin = 10;
        editor->move(selectionRect.topRight() + QPoint(margin, margin));
    }
}

void ScreenshotDisplay::drawArrow(QPainter& painter, const QPoint& start, const QPoint& end) {
    painter.drawLine(start, end);

    double angle = std::atan2(start.y() - end.y(), start.x() - end.x());

    const double arrowHeadLength = borderWidth * 2;
    const double arrowHeadAngle = M_PI / 6;

    QPoint arrowP1 = end + QPoint(std::cos(angle + arrowHeadAngle) * arrowHeadLength,
        std::sin(angle + arrowHeadAngle) * arrowHeadLength);
    QPoint arrowP2 = end + QPoint(std::cos(angle - arrowHeadAngle) * arrowHeadLength,
        std::sin(angle - arrowHeadAngle) * arrowHeadLength);

    QPolygon arrowHead;
    arrowHead << end << arrowP1 << arrowP2;

    painter.setBrush(QBrush(editor->getCurrentColor()));
    painter.drawPolygon(arrowHead);
}

void ScreenshotDisplay::drawBorderCircle(QPainter& painter, const QPoint& position) {
    painter.setPen(QPen(editor->getCurrentColor(), 2, Qt::SolidLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(position, borderWidth, borderWidth);
}

void ScreenshotDisplay::adjustTextEditSize() {
    QFontMetrics fm(textEdit->font());
    int width = fm.horizontalAdvance(textEdit->toPlainText().replace('\n', ' ')) + 10;
    textEdit->setFixedSize(width, textEdit->height());
}

void ScreenshotDisplay::finalizeTextEdit() {
    if (textEdit) {
        saveStateForUndo();
        QPainter painter(&drawingPixmap);
        painter.setFont(textEdit->font());
        painter.setPen(QPen(editor->getCurrentColor()));

        QFontMetrics fm(textEdit->font());
        QStringList lines = textEdit->toPlainText().split('\n');
        QPoint currentPos = textEditPosition;

        currentPos.setY(currentPos.y() + fm.ascent());

        for (const QString& line : lines) {
            painter.drawText(currentPos, line);
            currentPos.setY(currentPos.y() + fm.height());
        }

        textEdit->deleteLater();
        textEdit = nullptr;
        update();
    }
}

void ScreenshotDisplay::saveStateForUndo() {
    undoStack.push(drawingPixmap);
}

void ScreenshotDisplay::undo() {
    if (!undoStack.empty()) {
        drawingPixmap = undoStack.top();
        undoStack.pop();
        update();
    }
}
