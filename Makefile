# Variables pour faciliter les modifications futures
CXX = g++
CXXFLAGS = -w
LIBS = -lsqlite3 -lpthread -lssl -lcrypto
TARGET = serveur
APACHE_BIN = /home/goudale/Documents/rezo/bourse/bin/apachectl

# La règle par défaut (quand tu tapes juste 'make')
all: restart_apache compile run

# 1. Redémarrer Apache
restart_apache:
	@echo "🔄 Redémarrage d'Apache..."
	sudo $(APACHE_BIN) -k restart

# 2. Compiler le C++
compile:
	@echo "🔨 Compilation du serveur C++..."
	$(CXX) serveur.cpp $(CXXFLAGS) -o $(TARGET) $(LIBS)

# 3. Lancer le serveur (charge .env si présent)
run:
	@echo "🚀 Lancement du serveur..."
	@if [ -f .env ]; then export $$(cat .env | grep -v '^#' | xargs) && ./$(TARGET); else ./$(TARGET); fi

# Optionnel : pour nettoyer les fichiers binaires
clean:
	rm -f $(TARGET)