/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "extracompiler.h"

#include "buildconfiguration.h"
#include "buildmanager.h"
#include "kitinformation.h"
#include "session.h"
#include "target.h"

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/idocument.h>
#include <texteditor/texteditor.h>
#include <texteditor/texteditorsettings.h>
#include <texteditor/texteditorconstants.h>
#include <texteditor/fontsettings.h>

#include <utils/qtcassert.h>
#include <utils/runextensions.h>

#include <QDateTime>
#include <QFutureInterface>
#include <QFutureWatcher>
#include <QProcess>
#include <QThreadPool>
#include <QTimer>
#include <QTextBlock>

namespace ProjectExplorer {

Q_GLOBAL_STATIC(QThreadPool, s_extraCompilerThreadPool);
Q_GLOBAL_STATIC(QList<ExtraCompilerFactory *>, factories);
Q_GLOBAL_STATIC(QVector<ExtraCompilerFactoryObserver *>, observers);
class ExtraCompilerPrivate
{
public:
    const Project *project;
    Utils::FilePath source;
    FileNameToContentsHash contents;
    Tasks issues;
    QDateTime compileTime;
    Core::IEditor *lastEditor = nullptr;
    QMetaObject::Connection activeBuildConfigConnection;
    QMetaObject::Connection activeEnvironmentConnection;
    bool dirty = false;

    QTimer timer;
    void updateIssues();
};

ExtraCompiler::ExtraCompiler(const Project *project, const Utils::FilePath &source,
                             const Utils::FilePaths &targets, QObject *parent) :
    QObject(parent), d(std::make_unique<ExtraCompilerPrivate>())
{
    d->project = project;
    d->source = source;
    foreach (const Utils::FilePath &target, targets)
        d->contents.insert(target, QByteArray());
    d->timer.setSingleShot(true);

    connect(&d->timer, &QTimer::timeout, this, [this](){
        if (d->dirty && d->lastEditor) {
            d->dirty = false;
            run(d->lastEditor->document()->contents());
        }
    });

    connect(BuildManager::instance(), &BuildManager::buildStateChanged,
            this, &ExtraCompiler::onTargetsBuilt);

    connect(SessionManager::instance(), &SessionManager::projectRemoved,
            this, [this](Project *project) {
        if (project == d->project)
            deleteLater();
    });

    Core::EditorManager *editorManager = Core::EditorManager::instance();
    connect(editorManager, &Core::EditorManager::currentEditorChanged,
            this, &ExtraCompiler::onEditorChanged);
    connect(editorManager, &Core::EditorManager::editorAboutToClose,
            this, &ExtraCompiler::onEditorAboutToClose);

    // Use existing target files, where possible. Otherwise run the compiler.
    QDateTime sourceTime = d->source.lastModified();
    foreach (const Utils::FilePath &target, targets) {
        QFileInfo targetFileInfo(target.toFileInfo());
        if (!targetFileInfo.exists()) {
            d->dirty = true;
            continue;
        }

        QDateTime lastModified = targetFileInfo.lastModified();
        if (lastModified < sourceTime)
            d->dirty = true;

        if (!d->compileTime.isValid() || d->compileTime > lastModified)
            d->compileTime = lastModified;

        QFile file(target.toString());
        if (file.open(QFile::ReadOnly | QFile::Text))
            setContent(target, file.readAll());
    }

    if (d->dirty) {
        d->dirty = false;
        QTimer::singleShot(0, this, [this]() { run(d->source); }); // delay till available.
    }
}

ExtraCompiler::~ExtraCompiler() = default;

const Project *ExtraCompiler::project() const
{
    return d->project;
}

Utils::FilePath ExtraCompiler::source() const
{
    return d->source;
}

QByteArray ExtraCompiler::content(const Utils::FilePath &file) const
{
    return d->contents.value(file);
}

Utils::FilePaths ExtraCompiler::targets() const
{
    return d->contents.keys();
}

void ExtraCompiler::forEachTarget(std::function<void (const Utils::FilePath &)> func)
{
    for (auto it = d->contents.constBegin(), end = d->contents.constEnd(); it != end; ++it)
        func(it.key());
}

void ExtraCompiler::setCompileTime(const QDateTime &time)
{
    d->compileTime = time;
}

QDateTime ExtraCompiler::compileTime() const
{
    return d->compileTime;
}

QThreadPool *ExtraCompiler::extraCompilerThreadPool()
{
    return s_extraCompilerThreadPool();
}

void ExtraCompiler::onTargetsBuilt(Project *project)
{
    if (project != d->project || BuildManager::isBuilding(project))
        return;

    // This is mostly a fall back for the cases when the generator couldn't be run.
    // It pays special attention to the case where a source file was newly created
    const QDateTime sourceTime = d->source.lastModified();
    if (d->compileTime.isValid() && d->compileTime >= sourceTime)
        return;

    forEachTarget([&](const Utils::FilePath &target) {
        QFileInfo fi(target.toFileInfo());
        QDateTime generateTime = fi.exists() ? fi.lastModified() : QDateTime();
        if (generateTime.isValid() && (generateTime > sourceTime)) {
            if (d->compileTime >= generateTime)
                return;

            QFile file(target.toString());
            if (file.open(QFile::ReadOnly | QFile::Text)) {
                d->compileTime = generateTime;
                setContent(target, file.readAll());
            }
        }
    });
}

void ExtraCompiler::onEditorChanged(Core::IEditor *editor)
{
    // Handle old editor
    if (d->lastEditor) {
        Core::IDocument *doc = d->lastEditor->document();
        disconnect(doc, &Core::IDocument::contentsChanged,
                   this, &ExtraCompiler::setDirty);

        if (d->dirty) {
            d->dirty = false;
            run(doc->contents());
        }
    }

    if (editor && editor->document()->filePath() == d->source) {
        d->lastEditor = editor;
        d->updateIssues();

        // Handle new editor
        connect(d->lastEditor->document(), &Core::IDocument::contentsChanged,
                this, &ExtraCompiler::setDirty);
    } else {
        d->lastEditor = nullptr;
    }
}

void ExtraCompiler::setDirty()
{
    d->dirty = true;
    d->timer.start(1000);
}

void ExtraCompiler::onEditorAboutToClose(Core::IEditor *editor)
{
    if (d->lastEditor != editor)
        return;

    // Oh no our editor is going to be closed
    // get the content first
    Core::IDocument *doc = d->lastEditor->document();
    disconnect(doc, &Core::IDocument::contentsChanged,
               this, &ExtraCompiler::setDirty);
    if (d->dirty) {
        d->dirty = false;
        run(doc->contents());
    }
    d->lastEditor = nullptr;
}

Utils::Environment ExtraCompiler::buildEnvironment() const
{
    if (Target *target = project()->activeTarget()) {
        if (BuildConfiguration *bc = target->activeBuildConfiguration()) {
            return bc->environment();
        } else {
            Utils::EnvironmentItems changes =
                    EnvironmentKitAspect::environmentChanges(target->kit());
            Utils::Environment env = Utils::Environment::systemEnvironment();
            env.modify(changes);
            return env;
        }
    }

    return Utils::Environment::systemEnvironment();
}

void ExtraCompiler::setCompileIssues(const Tasks &issues)
{
    d->issues = issues;
    d->updateIssues();
}

void ExtraCompilerPrivate::updateIssues()
{
    if (!lastEditor)
        return;

    auto widget = qobject_cast<TextEditor::TextEditorWidget *>(lastEditor->widget());
    if (!widget)
        return;

    QList<QTextEdit::ExtraSelection> selections;
    const QTextDocument *document = widget->document();
    foreach (const Task &issue, issues) {
        QTextEdit::ExtraSelection selection;
        QTextCursor cursor(document->findBlockByNumber(issue.line - 1));
        cursor.movePosition(QTextCursor::StartOfLine);
        cursor.movePosition(QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
        selection.cursor = cursor;

        const auto fontSettings = TextEditor::TextEditorSettings::fontSettings();
        selection.format = fontSettings.toTextCharFormat(issue.type == Task::Warning ?
                TextEditor::C_WARNING : TextEditor::C_ERROR);
        selection.format.setToolTip(issue.description());
        selections.append(selection);
    }

    widget->setExtraSelections(TextEditor::TextEditorWidget::CodeWarningsSelection, selections);
}

void ExtraCompiler::setContent(const Utils::FilePath &file, const QByteArray &contents)
{
    auto it = d->contents.find(file);
    if (it != d->contents.end()) {
        if (it.value() != contents) {
            it.value() = contents;
            emit contentsChanged(file);
        }
    }
}

ExtraCompilerFactory::ExtraCompilerFactory(QObject *parent)
    : QObject(parent)
{
    factories->append(this);
}

ExtraCompilerFactory::~ExtraCompilerFactory()
{
    factories->removeAll(this);
}

void ExtraCompilerFactory::annouceCreation(const Project *project,
                                           const Utils::FilePath &source,
                                           const Utils::FilePaths &targets)
{
    for (ExtraCompilerFactoryObserver *observer : qAsConst(*observers))
        observer->newExtraCompiler(project, source, targets);
}

QList<ExtraCompilerFactory *> ExtraCompilerFactory::extraCompilerFactories()
{
    return *factories();
}

ProcessExtraCompiler::ProcessExtraCompiler(const Project *project, const Utils::FilePath &source,
                                           const Utils::FilePaths &targets, QObject *parent) :
    ExtraCompiler(project, source, targets, parent)
{ }

ProcessExtraCompiler::~ProcessExtraCompiler()
{
    if (!m_watcher)
        return;
    m_watcher->cancel();
    m_watcher->waitForFinished();
}

void ProcessExtraCompiler::run(const QByteArray &sourceContents)
{
    ContentProvider contents = [sourceContents]() { return sourceContents; };
    runImpl(contents);
}

void ProcessExtraCompiler::run(const Utils::FilePath &fileName)
{
    ContentProvider contents = [fileName]() {
        QFile file(fileName.toString());
        if (!file.open(QFile::ReadOnly | QFile::Text))
            return QByteArray();
        return file.readAll();
    };
    runImpl(contents);
}

Utils::FilePath ProcessExtraCompiler::workingDirectory() const
{
    return Utils::FilePath();
}

QStringList ProcessExtraCompiler::arguments() const
{
    return QStringList();
}

bool ProcessExtraCompiler::prepareToRun(const QByteArray &sourceContents)
{
    Q_UNUSED(sourceContents)
    return true;
}

Tasks ProcessExtraCompiler::parseIssues(const QByteArray &stdErr)
{
    Q_UNUSED(stdErr)
    return {};
}

void ProcessExtraCompiler::runImpl(const ContentProvider &provider)
{
    if (m_watcher)
        delete m_watcher;

    m_watcher = new QFutureWatcher<FileNameToContentsHash>();
    connect(m_watcher, &QFutureWatcher<FileNameToContentsHash>::finished,
            this, &ProcessExtraCompiler::cleanUp);

    m_watcher->setFuture(Utils::runAsync(extraCompilerThreadPool(),
                                         &ProcessExtraCompiler::runInThread, this,
                                         command(), workingDirectory(), arguments(), provider,
                                         buildEnvironment()));
}

void ProcessExtraCompiler::runInThread(
        QFutureInterface<FileNameToContentsHash> &futureInterface,
        const Utils::FilePath &cmd, const Utils::FilePath &workDir,
        const QStringList &args, const ContentProvider &provider,
        const Utils::Environment &env)
{
    if (cmd.isEmpty() || !cmd.toFileInfo().isExecutable())
        return;

    const QByteArray sourceContents = provider();
    if (sourceContents.isNull() || !prepareToRun(sourceContents))
        return;

    QProcess process;

    process.setProcessEnvironment(env.toProcessEnvironment());
    if (!workDir.isEmpty())
        process.setWorkingDirectory(workDir.toString());
    process.start(cmd.toString(), args, QIODevice::ReadWrite);
    if (!process.waitForStarted()) {
        handleProcessError(&process);
        return;
    }
    bool isCanceled = futureInterface.isCanceled();
    if (!isCanceled) {
        handleProcessStarted(&process, sourceContents);
        forever {
            bool done = process.waitForFinished(200);
            isCanceled = futureInterface.isCanceled();
            if (done || isCanceled)
                break;
        }
    }

    isCanceled |= process.state() == QProcess::Running;
    if (isCanceled) {
        process.kill();
        process.waitForFinished();
        return;
    }

    futureInterface.reportResult(handleProcessFinished(&process));
}

void ProcessExtraCompiler::cleanUp()
{
    QTC_ASSERT(m_watcher, return);
    auto future = m_watcher->future();
    delete m_watcher;
    m_watcher = nullptr;
    if (!future.resultCount())
        return;
    const FileNameToContentsHash data = future.result();

    if (data.isEmpty())
        return; // There was some kind of error...

    for (auto it = data.constBegin(), end = data.constEnd(); it != end; ++it)
        setContent(it.key(), it.value());

    setCompileTime(QDateTime::currentDateTime());
}

ExtraCompilerFactoryObserver::ExtraCompilerFactoryObserver()
{
    observers->push_back(this);
}

ExtraCompilerFactoryObserver::~ExtraCompilerFactoryObserver()
{
    observers->removeOne(this);
}

} // namespace ProjectExplorer
