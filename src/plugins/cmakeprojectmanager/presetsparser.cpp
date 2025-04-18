// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "presetsparser.h"

#include "cmakeprojectmanagertr.h"

#include <utils/algorithm.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace Utils;

namespace CMakeProjectManager::Internal {

bool parseVersion(const QJsonValue &jsonValue, int &version)
{
    if (jsonValue.isUndefined())
        return false;

    const int invalidVersion = -1;
    version = jsonValue.toInt(invalidVersion);
    return version != invalidVersion;
}

bool parseCMakeMinimumRequired(const QJsonValue &jsonValue, QVersionNumber &versionNumber)
{
    if (jsonValue.isUndefined() || !jsonValue.isObject())
        return false;

    QJsonObject object = jsonValue.toObject();
    versionNumber = QVersionNumber(object.value("major").toInt(),
                                   object.value("minor").toInt(),
                                   object.value("patch").toInt());

    return true;
}

std::optional<QStringList> parseInclude(const QJsonValue &jsonValue)
{
    std::optional<QStringList> includes;

    if (!jsonValue.isUndefined()) {
        if (jsonValue.isArray()) {
            includes = QStringList();
            const QJsonArray includeArray = jsonValue.toArray();
            for (const auto &includeValue : includeArray)
                includes.value() << includeValue.toString();
        }
    }

    return includes;
}

std::optional<PresetsDetails::Condition> parseCondition(const QJsonValue &jsonValue)
{
    std::optional<PresetsDetails::Condition> condition;

    if (jsonValue.isUndefined())
        return condition;

    condition = PresetsDetails::Condition();

    if (jsonValue.isNull()) {
        condition->type = "null";
        return condition;
    }

    if (jsonValue.isBool()) {
        condition->type = "const";
        condition->constValue = jsonValue.toBool();
        return condition;
    }

    if (!jsonValue.isObject())
        return condition;

    QJsonObject object = jsonValue.toObject();
    QString type = object.value("type").toString();
    if (type.isEmpty())
        return condition;

    if (type == "const") {
        condition->type = type;
        condition->constValue = object.value("value").toBool();
        return condition;
    }

    for (const auto &equals : {QString("equals"), QString("notEquals")}) {
        if (type == equals) {
            condition->type = equals;
            condition->lhs = object.value("lhs").toString();
            condition->rhs = object.value("rhs").toString();
        }
    }
    if (!condition->type.isEmpty())
        return condition;

    for (const auto &inList : {QString("inList"), QString("notInList")}) {
        if (type == inList) {
            condition->type = inList;
            condition->string = object.value("string").toString();
            if (object.value("list").isArray()) {
                condition->list = QStringList();
                const QJsonArray listArray = object.value("list").toArray();
                for (const auto &listValue : listArray)
                    condition->list.value() << listValue.toString();
            }
        }
    }
    if (!condition->type.isEmpty())
        return condition;

    for (const auto &matches : {QString("matches"), QString("notMatches")}) {
        if (type == matches) {
            condition->type = matches;
            condition->string = object.value("string").toString();
            condition->regex = object.value("regex").toString();
        }
    }
    if (!condition->type.isEmpty())
        return condition;

    for (const auto &anyOf : {QString("anyOf"), QString("allOf")}) {
        if (type == anyOf) {
            condition->type = anyOf;
            if (object.value("conditions").isArray()) {
                condition->conditions = std::vector<PresetsDetails::Condition::ConditionPtr>();
                const QJsonArray conditionsArray = object.value("conditions").toArray();
                for (const auto &conditionsValue : conditionsArray) {
                    condition->conditions.value().emplace_back(
                        std::make_shared<PresetsDetails::Condition>(
                            parseCondition(conditionsValue).value()));
                }
            }
        }
    }
    if (!condition->type.isEmpty())
        return condition;

    if (type == "not") {
        condition->type = type;
        condition->condition = std::make_shared<PresetsDetails::Condition>(
            parseCondition(object.value("condition")).value());
        return condition;
    }

    return condition;
}

bool parseVendor(const QJsonValue &jsonValue, std::optional<QVariantMap> &vendorSettings)
{
    // The whole section is optional
    if (jsonValue.isUndefined())
        return true;
    if (!jsonValue.isObject())
        return false;

    const QJsonObject object = jsonValue.toObject();
    const QJsonValue qtIo = object.value("qt.io/QtCreator/1.0");
    if (qtIo.isUndefined())
        return true;
    if (!qtIo.isObject())
        return false;

    const QJsonObject qtIoObject = qtIo.toObject();
    vendorSettings = QVariantMap();
    for (const QString &settingKey : qtIoObject.keys()) {
        const QJsonValue settingValue = qtIoObject.value(settingKey);
        vendorSettings->insert(settingKey, settingValue.toVariant());
    }
    return true;
}

bool parseConfigurePresets(const QJsonValue &jsonValue,
                           QList<PresetsDetails::ConfigurePreset> &configurePresets,
                           const Utils::FilePath &fileDir)
{
    // The whole section is optional
    if (jsonValue.isUndefined())
        return true;

    if (!jsonValue.isArray())
        return false;

    const QJsonArray configurePresetsArray = jsonValue.toArray();
    for (const auto &presetJson : configurePresetsArray) {
        if (!presetJson.isObject())
            continue;

        QJsonObject object = presetJson.toObject();
        PresetsDetails::ConfigurePreset preset;

        preset.name = object.value("name").toString();
        preset.fileDir = fileDir;
        preset.hidden = object.value("hidden").toBool();

        QJsonValue inherits = object.value("inherits");
        if (!inherits.isUndefined()) {
            preset.inherits = QStringList();
            if (inherits.isArray()) {
                const QJsonArray inheritsArray = inherits.toArray();
                for (const auto &inheritsValue : inheritsArray)
                    preset.inherits.value() << inheritsValue.toString();
            } else {
                QString inheritsValue = inherits.toString();
                if (!inheritsValue.isEmpty())
                    preset.inherits.value() << inheritsValue;
            }
        }

        if (object.contains("condition"))
            preset.condition = parseCondition(object.value("condition"));

        if (object.contains("vendor"))
            parseVendor(object.value("vendor"), preset.vendor);

        if (object.contains("displayName"))
            preset.displayName = object.value("displayName").toString();
        if (object.contains("description"))
            preset.description = object.value("description").toString();
        if (object.contains("generator"))
            preset.generator = object.value("generator").toString();
        if (object.contains("binaryDir"))
            preset.binaryDir = object.value("binaryDir").toString();
        if (object.contains("installDir"))
            preset.installDir = object.value("installDir").toString();
        if (object.contains("toolchainFile"))
            preset.toolchainFile = object.value("toolchainFile").toString();
        if (object.contains("cmakeExecutable"))
            preset.cmakeExecutable = FilePath::fromUserInput(object.value("cmakeExecutable").toString());

        const QJsonObject cacheVariablesObj = object.value("cacheVariables").toObject();
        for (const QString &cacheKey : cacheVariablesObj.keys()) {
            if (!preset.cacheVariables)
                preset.cacheVariables = CMakeConfig();

            QJsonValue cacheValue = cacheVariablesObj.value(cacheKey);
            if (cacheValue.isObject()) {
                QJsonObject cacheVariableObj = cacheValue.toObject();
                CMakeConfigItem item;
                item.key = cacheKey.toUtf8();
                item.type = CMakeConfigItem::typeStringToType(
                    cacheVariableObj.value("type").toString().toUtf8());
                item.value = cacheVariableObj.value("value").toString().toUtf8();
                preset.cacheVariables.value() << item;

            } else {
                if (cacheValue.isBool()) {
                    preset.cacheVariables.value()
                        << CMakeConfigItem(cacheKey.toUtf8(),
                                           CMakeConfigItem::BOOL,
                                           cacheValue.toBool() ? "ON" : "OFF");
                } else if (CMakeConfigItem::toBool(cacheValue.toString()).has_value()) {
                    preset.cacheVariables.value()
                        << CMakeConfigItem(cacheKey.toUtf8(),
                                           CMakeConfigItem::BOOL,
                                           cacheValue.toString().toUtf8());
                } else {
                    preset.cacheVariables.value()
                        << CMakeConfigItem(cacheKey.toUtf8(), cacheValue.toString().toUtf8());
                }
            }
        }

        const QJsonObject environmentObj = object.value("environment").toObject();
        for (const QString &envKey : environmentObj.keys()) {
            if (!preset.environment)
                preset.environment = Utils::Environment();

            QJsonValue envValue = environmentObj.value(envKey);
            preset.environment.value().set(envKey, envValue.toString());
        }

        const QJsonObject warningsObj = object.value("warnings").toObject();
        if (!warningsObj.isEmpty()) {
            preset.warnings = PresetsDetails::Warnings();

            if (warningsObj.contains("dev"))
                preset.warnings->dev = warningsObj.value("dev").toBool();
            if (warningsObj.contains("deprecated"))
                preset.warnings->deprecated = warningsObj.value("deprecated").toBool();
            if (warningsObj.contains("uninitialized"))
                preset.warnings->uninitialized = warningsObj.value("uninitialized").toBool();
            if (warningsObj.contains("unusedCli"))
                preset.warnings->unusedCli = warningsObj.value("unusedCli").toBool();
            if (warningsObj.contains("systemVars"))
                preset.warnings->systemVars = warningsObj.value("systemVars").toBool();
        }

        const QJsonObject errorsObj = object.value("errors").toObject();
        if (!errorsObj.isEmpty()) {
            preset.errors = PresetsDetails::Errors();

            if (errorsObj.contains("dev"))
                preset.errors->dev = errorsObj.value("dev").toBool();
            if (errorsObj.contains("deprecated"))
                preset.errors->deprecated = errorsObj.value("deprecated").toBool();
        }

        const QJsonObject debugObj = object.value("debug").toObject();
        if (!debugObj.isEmpty()) {
            preset.debug = PresetsDetails::Debug();

            if (debugObj.contains("output"))
                preset.debug->output = debugObj.value("output").toBool();
            if (debugObj.contains("tryCompile"))
                preset.debug->tryCompile = debugObj.value("tryCompile").toBool();
            if (debugObj.contains("find"))
                preset.debug->find = debugObj.value("find").toBool();
        }

        const QJsonObject architectureObj = object.value("architecture").toObject();
        if (!architectureObj.isEmpty()) {
            preset.architecture = PresetsDetails::ValueStrategyPair();

            if (architectureObj.contains("value"))
                preset.architecture->value = architectureObj.value("value").toString();
            if (architectureObj.contains("strategy")) {
                const QString strategy = architectureObj.value("strategy").toString();
                if (strategy == "set")
                    preset.architecture->strategy = PresetsDetails::ValueStrategyPair::Strategy::set;
                if (strategy == "external")
                    preset.architecture->strategy
                        = PresetsDetails::ValueStrategyPair::Strategy::external;
            } else {
                preset.architecture->strategy = PresetsDetails::ValueStrategyPair::Strategy::set;
            }
        } else {
            const QString value = object.value("architecture").toString();
            if (!value.isEmpty()) {
                preset.architecture = PresetsDetails::ValueStrategyPair();
                preset.architecture->value = value;
                preset.architecture->strategy = PresetsDetails::ValueStrategyPair::Strategy::set;
            }
        }

        const QJsonObject toolsetObj = object.value("toolset").toObject();
        if (!toolsetObj.isEmpty()) {
            preset.toolset = PresetsDetails::ValueStrategyPair();

            if (toolsetObj.contains("value"))
                preset.toolset->value = toolsetObj.value("value").toString();
            if (toolsetObj.contains("strategy")) {
                const QString strategy = toolsetObj.value("strategy").toString();
                if (strategy == "set")
                    preset.toolset->strategy = PresetsDetails::ValueStrategyPair::Strategy::set;
                if (strategy == "external")
                    preset.toolset->strategy = PresetsDetails::ValueStrategyPair::Strategy::external;
            } else {
                preset.toolset->strategy = PresetsDetails::ValueStrategyPair::Strategy::set;
            }
        } else {
            const QString value = object.value("toolset").toString();
            if (!value.isEmpty()) {
                preset.toolset = PresetsDetails::ValueStrategyPair();
                preset.toolset->value = value;
                preset.toolset->strategy = PresetsDetails::ValueStrategyPair::Strategy::set;
            }
        }

        configurePresets.emplace_back(preset);
    }

    return true;
}

static bool parseBuildPresets(const QJsonValue &jsonValue,
                              QList<PresetsDetails::BuildPreset> &buildPresets,
                              const FilePath &fileDir)
{
    // The whole section is optional
    if (jsonValue.isUndefined())
        return true;

    if (!jsonValue.isArray())
        return false;

    const QJsonArray buildPresetsArray = jsonValue.toArray();
    for (const auto &presetJson : buildPresetsArray) {
        if (!presetJson.isObject())
            continue;

        QJsonObject object = presetJson.toObject();
        PresetsDetails::BuildPreset preset;

        preset.name = object.value("name").toString();
        preset.fileDir = fileDir;
        preset.hidden = object.value("hidden").toBool();

        QJsonValue inherits = object.value("inherits");
        if (!inherits.isUndefined()) {
            preset.inherits = QStringList();
            if (inherits.isArray()) {
                const QJsonArray inheritsArray = inherits.toArray();
                for (const auto &inheritsValue : inheritsArray)
                    preset.inherits.value() << inheritsValue.toString();
            } else {
                QString inheritsValue = inherits.toString();
                if (!inheritsValue.isEmpty())
                    preset.inherits.value() << inheritsValue;
            }
        }

        if (object.contains("condition"))
            preset.condition = parseCondition(object.value("condition"));

        if (object.contains("vendor"))
            parseVendor(object.value("vendor"), preset.vendor);

        if (object.contains("displayName"))
            preset.displayName = object.value("displayName").toString();
        if (object.contains("description"))
            preset.description = object.value("description").toString();

        const QJsonObject environmentObj = object.value("environment").toObject();
        for (const QString &envKey : environmentObj.keys()) {
            if (!preset.environment)
                preset.environment = Utils::Environment();

            QJsonValue envValue = environmentObj.value(envKey);
            preset.environment.value().set(envKey, envValue.toString());
        }

        if (object.contains("configurePreset"))
            preset.configurePreset = object.value("configurePreset").toString();
        if (object.contains("inheritConfigureEnvironment"))
            preset.inheritConfigureEnvironment = object.value("inheritConfigureEnvironment").toBool();
        if (object.contains("jobs"))
            preset.jobs = object.value("jobs").toInt();

        QJsonValue targets = object.value("targets");
        if (!targets.isUndefined()) {
            preset.targets = QStringList();
            if (targets.isArray()) {
                const QJsonArray targetsArray = targets.toArray();
                for (const auto &targetsValue : targetsArray)
                    preset.targets.value() << targetsValue.toString();
            } else {
                QString targetsValue = targets.toString();
                if (!targetsValue.isEmpty())
                    preset.targets.value() << targetsValue;
            }
        }
        if (object.contains("configuration"))
            preset.configuration = object.value("configuration").toString();
        if (object.contains("verbose"))
            preset.verbose = object.value("verbose").toBool();
        if (object.contains("cleanFirst"))
            preset.cleanFirst = object.value("cleanFirst").toBool();

        QJsonValue nativeToolOptions = object.value("nativeToolOptions");
        if (!nativeToolOptions.isUndefined()) {
            if (nativeToolOptions.isArray()) {
                preset.nativeToolOptions = QStringList();
                const QJsonArray toolOptionsArray = nativeToolOptions.toArray();
                for (const auto &toolOptionsValue : toolOptionsArray)
                    preset.nativeToolOptions.value() << toolOptionsValue.toString();
            }
        }

        buildPresets.emplace_back(preset);
    }

    return true;
}

const PresetsData &PresetsParser::presetsData() const
{
    return m_presetsData;
}

bool PresetsParser::parse(const FilePath &jsonFile, QString &errorMessage, int &errorLine)
{
    const Utils::Result<QByteArray> jsonContents = jsonFile.fileContents();
    if (!jsonContents) {
        errorMessage
            = ::CMakeProjectManager::Tr::tr("Failed to read file \"%1\".").arg(jsonFile.fileName());
        return false;
    }

    QJsonParseError error;
    const QJsonDocument jsonDoc = QJsonDocument::fromJson(jsonContents.value(), &error);
    if (jsonDoc.isNull()) {
        errorLine = 1;
        for (int i = 0; i < error.offset; ++i)
            if (jsonContents.value().at(i) == '\n')
                ++errorLine;
        errorMessage = error.errorString();
        return false;
    }

    if (!jsonDoc.isObject()) {
        errorMessage
            = ::CMakeProjectManager::Tr::tr("Invalid file \"%1\".").arg(jsonFile.fileName());
        return false;
    }

    QJsonObject root = jsonDoc.object();

    m_presetsData.fileDir = jsonFile.parentDir();

    if (!parseVersion(root.value("version"), m_presetsData.version)) {
        errorMessage = ::CMakeProjectManager::Tr::tr("Invalid \"version\" in file \"%1\".")
                           .arg(jsonFile.fileName());
        return false;
    }

    // optional
    parseCMakeMinimumRequired(root.value("cmakeMinimumRequired"),
                              m_presetsData.cmakeMinimimRequired);

    // optional
    m_presetsData.include = parseInclude(root.value("include"));

    // optional
    if (!parseConfigurePresets(root.value("configurePresets"),
                               m_presetsData.configurePresets,
                               jsonFile.parentDir())) {
        errorMessage = ::CMakeProjectManager::Tr::tr(
                           "Invalid \"configurePresets\" section in file \"%1\".")
                           .arg(jsonFile.fileName());
        return false;
    }

    // optional
    if (!parseBuildPresets(root.value("buildPresets"),
                           m_presetsData.buildPresets,
                           jsonFile.parentDir())) {
        errorMessage = ::CMakeProjectManager::Tr::tr(
                           "Invalid \"buildPresets\" section in file \"%1\".")
                           .arg(jsonFile.fileName());
        return false;
    }

    // optional
    if (!parseVendor(root.value("vendor"), m_presetsData.vendor)) {
        errorMessage = ::CMakeProjectManager::Tr::tr("Invalid \"vendor\" section in file \"%1\".")
                           .arg(jsonFile.fileName());
    }

    return true;
}

static QVariantMap merge(const QVariantMap &first, const QVariantMap &second)
{
    QVariantMap result = first;
    for (auto it = second.constKeyValueBegin(); it != second.constKeyValueEnd(); ++it) {
        result[it->first] = it->second;
    }

    return result;
}

static CMakeConfig merge(const CMakeConfig &first, const CMakeConfig &second)
{
    return Utils::setUnionMerge<CMakeConfig>(
        first,
        second,
        [](const auto & /*left*/, const auto &right) { return right; },
        &CMakeConfigItem::less);
}

static QStringList merge(const QStringList &first, const QStringList &second)
{
    return Utils::setUnionMerge<QStringList>(
        first,
        second,
        [](const auto & /*left*/, const auto &right) { return right; });
}

void PresetsDetails::ConfigurePreset::inheritFrom(const ConfigurePreset &other)
{
    if (!condition && other.condition && !other.condition->isNull())
        condition = other.condition;

    if (!vendor && other.vendor)
        vendor = other.vendor;

    if (vendor && other.vendor)
        vendor = merge(*other.vendor, *vendor);

    if (!generator && other.generator)
        generator = other.generator;

    if (!architecture && other.architecture)
        architecture = other.architecture;

    if (!toolset && other.toolset)
        toolset = other.toolset;

    if (!toolchainFile && other.toolchainFile)
        toolchainFile = other.toolchainFile;

    if (!binaryDir && other.binaryDir)
        binaryDir = other.binaryDir;

    if (!installDir && other.installDir)
        installDir = other.installDir;

    if (!cmakeExecutable && other.cmakeExecutable)
        cmakeExecutable = other.cmakeExecutable;

    if (!cacheVariables && other.cacheVariables)
        cacheVariables = other.cacheVariables;
    else if (cacheVariables && other.cacheVariables)
        cacheVariables = merge(*other.cacheVariables, *cacheVariables);

    if (!environment && other.environment)
        environment = other.environment;
    else if (environment && other.environment)
        environment = environment->appliedToEnvironment(*other.environment);

    if (!warnings && other.warnings)
        warnings = other.warnings;

    if (!errors && other.errors)
        errors = other.errors;

    if (!debug && other.debug)
        debug = other.debug;
}

void PresetsDetails::BuildPreset::inheritFrom(const BuildPreset &other)
{
    if (!condition && other.condition && !other.condition->isNull())
        condition = other.condition;

    if (!vendor && other.vendor)
        vendor = other.vendor;

    if (vendor && other.vendor)
        vendor = merge(*other.vendor, *vendor);

    if (!environment && other.environment)
        environment = other.environment;
    else if (environment && other.environment)
        environment = environment->appliedToEnvironment(*other.environment);

    if (!configurePreset && other.configurePreset)
        configurePreset = other.configurePreset;

    if (!inheritConfigureEnvironment && other.inheritConfigureEnvironment)
        inheritConfigureEnvironment = other.inheritConfigureEnvironment;

    if (!jobs && other.jobs)
        jobs = other.jobs;

    if (!targets && other.targets)
        targets = other.targets;
    else if (targets && other.targets)
        targets = merge(*other.targets, *targets);

    if (!configuration && other.configuration)
        configuration = other.configuration;

    if (!verbose && other.verbose)
        verbose = other.verbose;

    if (!cleanFirst && other.cleanFirst)
        cleanFirst = other.cleanFirst;

    if (!nativeToolOptions && other.nativeToolOptions)
        nativeToolOptions = other.nativeToolOptions;
    else if (nativeToolOptions && other.nativeToolOptions)
        nativeToolOptions = merge(*other.nativeToolOptions, *nativeToolOptions);
}

bool PresetsDetails::Condition::evaluate() const
{
    if (isNull())
        return true;

    if (isConst() && constValue)
        return *constValue;

    if (isEquals() && lhs && rhs)
        return *lhs == *rhs;

    if (isNotEquals() && lhs && rhs)
        return *lhs != *rhs;

    if (isInList() && string && list)
        return list->contains(*string);

    if (isNotInList() && string && list)
        return !list->contains(*string);

    if (isMatches() && string && regex) {
        QRegularExpression qRegex(*regex);
        return qRegex.match(*string).hasMatch();
    }

    if (isNotMatches() && string && regex) {
        QRegularExpression qRegex(*regex);
        return !qRegex.match(*string).hasMatch();
    }

    if (isAnyOf() && conditions)
        return Utils::anyOf(*conditions, [](const ConditionPtr &c) { return c->evaluate(); });

    if (isAllOf() && conditions)
        return Utils::allOf(*conditions, [](const ConditionPtr &c) { return c->evaluate(); });

    if (isNot() && condition)
        return !(*condition)->evaluate();

    return false;
}

} // CMakeProjectManager::Internal
