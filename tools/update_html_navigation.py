#!/usr/bin/env python3
"""Два каталога html/: --check проверяет, --write обновляет статическую навигацию.

HTML содержит прямые ссылки на каталоги и резервные списки статей. Общие меню и списки
на сайте подгружаются из html/navigation-menu.html.
После правок схем или для обновления резервных списков запустите --write.
"""

import argparse
import difflib
import html
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
HTML = ROOT / "html"
CATALOG = "documentation.html"
USER_CATALOG = "instructions.html"
STANDALONE_PAGES = {"etcp-control-lab.html", "navigation-menu.html"}


def read_collections():
    """Взять разметку резервных каталогов из того же файла, который загружает сайт."""
    source = (HTML / "navigation-menu.html").read_text()
    collections = {}
    for slug, title, markup in re.findall(r'<template id="([\w-]+)" data-title="([^"]+)">(.*?)</template>', source, re.S):
        sections = [(section.attrib["id"], section.findtext("h2"), [link.attrib["href"] for link in section.findall("ul/li/a")])
                    for section in ET.fromstring(f"<catalog>{markup}</catalog>").findall("section")]
        collections[f"{slug}.html"] = (title, markup.strip("\n"), sections)
    if set(collections) != {CATALOG, USER_CATALOG}:
        raise ValueError("В navigation-menu.html ожидаются шаблоны documentation и instructions")
    return collections


def escape(text):
    """Экранировать текст или значение HTML-атрибута."""
    return html.escape(text, quote=True)


def block(name, content):
    """Пометить только генерируемый участок; остальное содержимое статьи сохраняется."""
    return f"<!-- docs:{name}:start -->\n{content}\n<!-- docs:{name}:end -->"


def navigation_assets(asset_query=""):
    return block("assets", f'''<link rel="stylesheet" href="navigation.css{asset_query}">
<script src="navigation.js{asset_query}" defer></script>''')


def header(catalog=None, asset_query=""):
    """Шапка с прямыми ссылками на оба каталога. Меню добавляет navigation.js."""
    user_current = ' aria-current="page"' if catalog == USER_CATALOG else ""
    tech_current = ' aria-current="page"' if catalog == CATALOG else ""
    return block("header", f'''<header class="header docs-header">
    <div class="container header-inner">
        <a class="brand" href="index.html" aria-label="VibeChat — на главную">
            <img src="assets/logo.png{asset_query}" alt="" width="36" height="36">
            <span>VibeChat<span class="brand-dot">.</span></span>
        </a>
        <nav class="docs-top" aria-label="Основная навигация">
            <a href="{USER_CATALOG}"{user_current}>Инструкции</a>
            <a href="{CATALOG}"{tech_current}>Разработчикам</a>
        </nav>
    </div>
</header>''')


def context(collection, article=False):
    """Путь к каталогу доступен до загрузки общих меню."""
    title = "Техническая документация" if collection == CATALOG else "Инструкции"
    trail = f'<li><a href="{collection}">{title}</a></li>' if article else f'<li aria-current="page">{title}</li>'
    return block("context", f'''<div class="docs-context">
    <nav class="docs-breadcrumbs" aria-label="Путь к странице">
        <ol><li><a href="index.html">Главная</a></li>
            {trail}
        </ol>
    </nav>
</div>''')


def pager(collection):
    return block("pager", f'''<nav class="docs-pager" aria-label="Другие страницы раздела">
    <a class="docs-pager-index" href="{collection}">Все разделы</a>
</nav>''')


def architecture_map():
    """Взять SVG с карты архитектуры и заменить ссылки на первые статьи из пояснений."""
    source = (HTML / "architecture.html").read_text()
    sections = re.findall(r'<section\b[^>]*\bdata-diagram="([^"]+)"[^>]*>(.*?)</section>', source, re.S)
    if [slug for slug, _ in sections] != ["network", "chat"]:
        raise ValueError("Карта архитектуры должна содержать схемы network и chat по порядку")

    figures, pages = [], {}
    for slug, section in sections:
        art = re.search(r'<svg class="architecture-svg"[^>]*>.*?</svg>', section, re.S)
        if not art:
            raise ValueError(f"Схема {slug}: отсутствует SVG")
        svg = ET.fromstring(art[0])
        targets = {}
        for module, article in re.findall(r'<article id="(module-[\w-]+)"[^>]*>(.*?)</article>', section, re.S):
            link = re.search(r'<div class="module-docs">\s*<a href="([^"]+)"', article)
            if not link or module in targets:
                raise ValueError(f"Схема {slug}: отсутствует ссылка или повторяется описание {module}")
            targets["#" + module] = link[1]

        for node in svg.iter("a"):
            module = node.get("href")
            if slug == "chat" and module == "#network":
                target = "utun.html"
                node.findall("text")[-1].text = "Обзор uTun →"
            else:
                target = targets.pop(module, None)
                if not target:
                    raise ValueError(f"Схема {slug}: нет статьи для блока {module}")
            filename, _, anchor = target.partition("#")
            if not re.fullmatch(r"[\w-]+\.html", filename):
                raise ValueError(f"Схема {slug}: блок {module} ссылается за пределы каталога ({target})")
            if filename not in pages:
                pages[filename] = (HTML / filename).read_text()
            if anchor and not re.search(r'\sid=["\']' + re.escape(anchor) + r'["\']', pages[filename]):
                raise ValueError(f"Схема {slug}: якорь блока {module} не найден ({target})")
            node.set("href", target)
        if targets:
            raise ValueError(f"Схема {slug}: нет блоков для {', '.join(targets)}")

        label = escape(svg.attrib["aria-label"])
        title = "Сетевое ядро uTun" if slug == "network" else "Модули и интерфейсы VibeChat"
        figures.append(f'''            <figure class="architecture-figure" aria-labelledby="architecture-{slug}">
                <h3 id="architecture-{slug}">{title}</h3>
                <div class="architecture-scroll" tabindex="0" role="region" aria-label="{label}, с прокруткой">
                    {ET.tostring(svg, encoding="unicode")}
                </div>
            </figure>''')
    return block("architecture", f'''        <section class="docs-architecture" id="architecture-map" aria-labelledby="architecture-map-title">
            <h2 id="architecture-map-title">Карта архитектуры</h2>
            <p>Выберите блок, чтобы открыть его описание. На узком экране схемы прокручиваются вбок.</p>
            <div class="architecture-legend" aria-label="Обозначения на диаграммах">
                <span><i class="legend-line" aria-hidden="true"></i>Сплошные стрелки — передача данных</span>
                <span><i class="legend-line legend-control" aria-hidden="true"></i>Пунктир — управление или маршруты</span>
            </div>
{chr(10).join(figures)}
        </section>''') + "\n"


def catalog_page(collection, title, markup, sections):
    """Статический каталог всех тем; поиск лишь дополняет обычные ссылки."""
    technical = collection == CATALOG
    shortcuts = ['            <a href="#architecture-map">Схемы</a>'] if technical else []
    architecture_css = '    <link rel="stylesheet" href="architecture.css">\n' if technical else ""
    diagrams = architecture_map() if technical else ""
    heading = "Как устроены VibeChat и uTun." if technical else "Как пользоваться VibeChat."
    subtitle = "Архитектура, механизмы и протоколы." if technical else "Скриншоты. Подсказки. Первые действия."
    lead = ("Выберите тему на схеме или найдите статью по названию и описанию. "
            "Здесь собраны технические механизмы, API и форматы обмена." if technical else
            "Выберите задачу. В руководствах показаны настоящие окна приложения, назначения кнопок и порядок действий.")
    for slug, label, _ in sections:
        shortcuts.append(f'            <a href="#{slug}">{escape(label)}</a>')
    return f'''<!doctype html>
<html lang="ru">
<head>
    <meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
    <meta name="description" content="{escape(lead)}">
    <title>{title} — VibeChat</title>
    <link rel="icon" href="assets/logo.png" type="image/png">
    <link rel="stylesheet" href="styles.css"><link rel="stylesheet" href="streams.css">
{architecture_css}    {navigation_assets()}
</head>
<body id="top"{(' class="docs-technical"' if technical else '')}>
    <a class="skip-link" href="#main">Перейти к содержимому</a>
{header(catalog=collection)}
{context(collection)}
    <main id="main" class="docs-catalog">
        <div class="stream-intro">
            <p class="eyebrow"><span class="little-dot"></span> VibeChat / {title}</p>
            <h1>{heading}<br><span>{subtitle}</span></h1>
            <p class="stream-lead">{lead}</p>
        </div>
        <div class="docs-search" hidden>
            <label for="docs-search">Найти страницу</label>
            <input id="docs-search" type="search" placeholder="{'Например: Merkle, маршруты, файлы' if technical else 'Например: звонок, буфер, приглашение'}" autocomplete="off" aria-describedby="docs-search-status">
            <p id="docs-search-status" role="status">Поиск по названиям и описаниям страниц.</p>
        </div>
        <nav class="docs-shortcuts" aria-label="Разделы документации">
{chr(10).join(shortcuts)}
        </nav>
{diagrams}        <div class="docs-categories">
{markup}
        </div>
    </main>
    <footer class="footer container"><a href="index.html">Главная VibeChat</a>
        <p>{title}</p><a class="back-top" href="#top">Наверх ↑</a></footer>
</body>
</html>
'''


def replace_block(text, name, content):
    """Заменить единственный служебный блок, не затрагивая соседний текст."""
    pattern = rf"<!-- docs:{name}:start -->.*?<!-- docs:{name}:end -->"
    matches = list(re.finditer(pattern, text, re.S))
    if len(matches) != 1:
        raise ValueError(f"Ожидался один блок {name}, найдено {len(matches)}")
    return text[:matches[0].start()] + content + text[matches[0].end():]


def update_page(text, page=None, collection=USER_CATALOG, asset_query=""):
    """Обновить рамку существующей статьи, оставив её текст и локальное оглавление."""
    assets = navigation_assets(asset_query)
    if "<!-- docs:assets:start -->" in text:
        text = replace_block(text, "assets", assets)
    else:
        if text.count("</head>") != 1:
            raise ValueError("Ожидалось одно окончание head")
        text = text.replace("</head>", assets + "\n</head>", 1)
    new_header = header(asset_query=asset_query)
    new_context = context(collection, article=True) if page else ""
    if collection == CATALOG:
        body = re.search(r"<body\b[^>]*>", text)
        if not body:
            raise ValueError("Отсутствует body технической статьи")
        tag = body.group()
        classes = re.search(r'class="([^"]*)"', tag)
        if not classes:
            tag = tag[:-1] + ' class="docs-technical">'
        elif "docs-technical" not in classes[1].split():
            tag = tag[:classes.start(1)] + "docs-technical " + tag[classes.start(1):]
        text = text[:body.start()] + tag + text[body.end():]
    if "<!-- docs:header:start -->" in text:
        text = replace_block(text, "header", new_header)
        if page:
            if "<!-- docs:context:start -->" in text:
                text = replace_block(text, "context", new_context)
            else:
                text = text.replace(new_header, new_header + "\n" + new_context, 1)
    else:
        headers = list(re.finditer(r"<header\b.*?</header>", text, re.S))
        if len(headers) == 1:
            old = headers[0]
            text = text[:old.start()] + new_header + ("\n" + new_context if page else "") + text[old.end():]
        elif not headers and page and page == "database-api.html":
            # Эта статья использует собственный стиль и раньше не имела общей шапки.
            text, count = re.subn(r'<nav aria-label="Документация">.*?</nav>\n', "", text, count=1, flags=re.S)
            if count != 1 or text.count("<body><main>") != 1:
                raise ValueError("Не распознана автономная статья базы данных")
            text = text.replace("<body><main>", '<body id="top">\n<a class="skip-link" href="#main">Перейти к содержимому</a>\n'
                                + new_header + "\n" + new_context + '\n<main id="main">', 1)
        else:
            raise ValueError(f"Ожидалась одна шапка, найдено {len(headers)}")
    if page:
        new_pager = pager(collection)
        if "<!-- docs:pager:start -->" in text:
            text = replace_block(text, "pager", new_pager)
        else:
            if text.count("</main>") != 1:
                raise ValueError("Ожидалось одно окончание main")
            text = text.replace("</main>", new_pager + "\n</main>", 1)
    return text


def outputs(collections):
    """Проверить полноту каталога до подготовки каких-либо изменений."""
    pages = [page for _, _, sections in collections.values() for _, _, links in sections for page in links]
    if len(pages) != len(set(pages)):
        raise ValueError("Страница указана в нескольких разделах")
    expected = set(pages) | {"index.html", *collections} | STANDALONE_PAGES
    actual = {path.name for path in HTML.glob("*.html")} | set(collections)
    if expected != actual:
        raise ValueError(f"Каталог не совпадает с файлами: лишние={sorted(actual - expected)}, отсутствуют={sorted(expected - actual)}")
    result = {HTML / name: catalog_page(name, *template) for name, template in collections.items()}
    index_path = HTML / "index.html"
    index = index_path.read_text()
    version = re.search(r'href="navigation\.css(\?v=[\w.-]+)"', index)
    result[index_path] = update_page(index, asset_query=version[1] if version else "")
    for collection, (_, _, sections) in collections.items():
        for _, _, links in sections:
            for page in links:
                path = HTML / page
                result[path] = update_page(path.read_text(), page, collection)
    return result


def main():
    """Проверка по умолчанию; запись или подготовка точечных патчей — явно."""
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="проверить актуальность без изменения файлов (по умолчанию)")
    mode.add_argument("--write", action="store_true", help="обновить каталог и служебные блоки навигации")
    mode.add_argument("--patch-dir", type=Path, help="подготовить патч для каждого изменяемого файла")
    args = parser.parse_args()
    try:
        collections = read_collections()
        generated = outputs(collections)
        changed = 0
        for path, content in generated.items():
            old = path.read_text() if path.exists() else ""
            if old == content:
                continue
            changed += 1
            relative = path.relative_to(ROOT)
            if args.write:
                path.write_text(content)
            elif args.patch_dir:
                args.patch_dir.mkdir(parents=True, exist_ok=True)
                if path.exists():
                    diff = list(difflib.unified_diff(old.splitlines(True), content.splitlines(True), n=3))
                    hunks = "".join("@@\n" if line.startswith("@@") else line for line in diff[2:])
                    patch = f"*** Begin Patch\n*** Update File: {relative}\n" + hunks + "*** End Patch\n"
                else:
                    patch = f"*** Begin Patch\n*** Add File: {relative}\n" + "".join("+" + line for line in content.splitlines(True)) + "*** End Patch\n"
                (args.patch_dir / (path.name + ".patch")).write_text(patch)
            else:
                print(f"Неактуальная навигация: {relative}", file=sys.stderr)
        sections = sum(len(sections) for _, _, sections in collections.values())
        print(f"Навигация: {len(generated)} страниц, {len(collections)} каталога, {sections} разделов, изменений: {changed}")
        return 1 if changed and not args.write and not args.patch_dir else 0
    except (OSError, ValueError, ET.ParseError) as error:
        print(f"Ошибка подготовки навигации: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
