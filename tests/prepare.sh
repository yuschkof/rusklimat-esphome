#!/bin/sh
# Делает из rusklimat-ac.yaml пакет для тестовых сборок: компонент берётся из этого репозитория, а не с GitHub.
set -e
cd "$(dirname "$0")"
sed -e 's#- source: github://yuschkof/rusklimat-esphome@.*#- source:\n      type: local\n      path: ../components#' \
  ../rusklimat-ac.yaml > _example.yaml
