import W
import sys

__version__ = "0.2"
__author__ = "jvr"

class _modulebrowser:
	
	def __init__(self):
		self.editmodules = []
		self.modules = []
		self.window = W.Window((194, 1000), "Module Browser", minsize = (194, 160), maxsize = (340, 20000))
		
		self.window.openbutton = W.Button((10, 8, 80, 16), "Open", self.openbuttonhit)
		self.window.browsebutton = W.Button((100, 8, 80, 16), "Browse�", self.browsebuttonhit)
		self.window.reloadbutton = W.Button((10, 32, 80, 16), "Reload", self.reloadbuttonhit)
		self.window.openotherbutton = W.Button((100, 32, 80, 16), "Open other�", self.openother)
		
		self.window.openbutton.enable(0)
		self.window.reloadbutton.enable(0)
		self.window.browsebutton.enable(0)
		self.window.setdefaultbutton(self.window.browsebutton)
		
		self.window.bind("cmdr", self.window.reloadbutton.push)
		self.window.bind("cmdb", self.window.browsebutton.push)
	
		self.window.bind("<activate>", self.activate)
		self.window.bind("<close>", self.close)
		
		self.window.list = W.List((-1, 56, 1, -14), [], self.listhit)
		
		self.window.open()
		self.checkbuttons()
	
	def close(self):
		global _browser
		_browser = None
	
	def activate(self, onoff):
		if onoff:
			self.makelist()
		
	def listhit(self, isdbl):
		self.checkbuttons()
		if isdbl:
			if self.window._defaultbutton:
				self.window._defaultbutton.push()
	
	def checkbuttons(self):
		sel = self.window.list.getselection()
		if sel:
			for i in sel:
				if self.editmodules[i]:
					self.window.openbutton.enable(1)
					self.window.reloadbutton.enable(1)
					self.window.setdefaultbutton(self.window.openbutton)
					break
			else:
				self.window.openbutton.enable(0)
				self.window.reloadbutton.enable(0)
				self.window.setdefaultbutton(self.window.browsebutton)
			self.window.browsebutton.enable(1)
		else:
			#self.window.setdefaultbutton(self.window.browsebutton)
			self.window.openbutton.enable(0)
			self.window.reloadbutton.enable(0)
			self.window.browsebutton.enable(0)
	
	def openbuttonhit(self):
		import imp
		sel = self.window.list.getselection()
		W.SetCursor("watch")
		for i in sel:
			modname = self.window.list[i]
			try:
				self.openscript(sys.modules[modname].__file__, modname)
			except IOError:
				try:
					file, path, description = imp.find_module(modname)
				except ImportError:
					W.SetCursor("arrow")
					W.Message("Can�t find file for module �%s�." 
							% modname)
				else:
					self.openscript(path, modname)					
	
	def openscript(self, path, modname):
		if path[-3:] == '.py':
			W.getapplication().openscript(path)
		else:
			W.Message("Can�t edit �%s�; it might be a shared library or a .pyc file." 
					% modname)
	
	def openother(self):
		import imp
		import EasyDialogs
		
		modname = EasyDialogs.AskString("Open module:")
		if modname:
			try:
				file, path, description = imp.find_module(modname)
			except ImportError:
				if modname in sys.builtin_module_names:
					alerttext = "�%s� is a builtin module, which you can�t edit." % modname
				else:
					alerttext = "No module named �%s�." % modname
				raise W.AlertError, alerttext
			self.openscript(path, modname)
	
	def reloadbuttonhit(self):
		sel = self.window.list.getselection()
		W.SetCursor("watch")
		for i in sel:
			m = sys.modules[self.window.list[i]]
			reload(m)
	
	def browsebuttonhit(self):
		sel = self.window.list.getselection()
		if not sel:
			return
		import PyBrowser
		for i in sel:
			PyBrowser.Browser(sys.modules[self.window.list[i]])
	
	def makelist(self):
		editmodules, modules = getmoduleslist()
		if modules == self.modules:
			return
		self.editmodules, self.modules = editmodules, modules
		self.window.list.setdrawingmode(0)
		sel = self.window.list.getselectedobjects()
		self.window.list.set(self.modules)
		self.window.list.setselectedobjects(sel)
		self.window.list.setdrawingmode(1)


def getmoduleslist():
	import PyBrowser	# for caselesssort function
	modules = sys.modules.keys()
	modules = PyBrowser.caselesssort(modules)
	editmodules = []
	sysmodules = sys.modules
	modulesappend = editmodules.append
	for m in modules:
		module = sysmodules[m]
		try:
			if sysmodules[m].__file__[-3:] == '.py':
				modulesappend(1)
			else:
				modulesappend(0)
		except AttributeError:
			modulesappend(0)
	return editmodules, modules
	
	

_browser = None

def ModuleBrowser():
	global _browser
	if _browser is not None:
		_browser.window.select()
	else:
		_browser = _modulebrowser()
