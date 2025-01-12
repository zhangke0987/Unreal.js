#include "V8PCH.h"
#include "JavascriptIsolate.h"
#include "JavascriptContext.h"
#include "JavascriptComponent.h"
#include "FileManager.h"
#include "Config.h"
#include "Translator.h"
#include "Exception.h"
#include "IV8.h"

#include "JavascriptIsolate_Private.h"
#include "JavascriptContext_Private.h"

#if WITH_EDITOR
#include "TypingGenerator.h"
#endif

#include "Helpers.h"
#include "JavascriptGeneratedClass_Native.h"
#include "JavascriptGeneratedClass.h"
#include "JavascriptGeneratedFunction.h"

using namespace v8;

static const int kContextEmbedderDataIndex = 1;
static const int32 MagicNumber = 0x2852abd3;

static UProperty* CreateProperty(UObject* Outer, FString Decl)
{
	TArray<FString> SmallArray;
	const TCHAR* Delims[] = {
		TEXT("/*"),
		TEXT("*/"),
		TEXT("+")
	};
	Decl.ParseIntoArray(SmallArray, Delims, ARRAY_COUNT(Delims));

	if (SmallArray.Num() < 2)
	{
		return nullptr;
	}

	FName Name = *SmallArray[0];
	SmallArray.RemoveAt(0, 1);
	FString Type = SmallArray.Pop();	
	
	auto SetupProperty = [&](UProperty* NewProperty) {
		static struct FKeyword {
			const TCHAR* Keyword;
			uint64 Flags;
		} Keywords[] = {
			{ TEXT("Const"), CPF_ConstParm },
			{ TEXT("Return"), CPF_ReturnParm },
			{ TEXT("Out"), CPF_OutParm },
			{ TEXT("Replicated"), CPF_Net },
			{ TEXT("NotReplicated"), CPF_RepSkip },
			{ TEXT("ReplicatedUsing"), CPF_Net | CPF_RepNotify },
			{ TEXT("EditAnywhere"), CPF_Edit },
			{ TEXT("EditDefaultsOnly"), CPF_Edit | CPF_DisableEditOnInstance },
			{ TEXT("VisibleAnywhere"), CPF_Edit | CPF_EditConst },
			{ TEXT("VisibleInstanceOnly"), CPF_Edit | CPF_EditConst | CPF_DisableEditOnTemplate },
			{ TEXT("VisibleDefaultsOnly"), CPF_Edit | CPF_EditConst | CPF_DisableEditOnInstance },
		};

		for (const auto& Flag : SmallArray)
		{
			FString Left, Right;
			if (!Flag.Split(TEXT(":"), &Left, &Right))
			{
				Left = Flag;
			}

			for (const auto& Keyword : Keywords)
			{	
				if (Left.Compare(Keyword.Keyword,ESearchCase::IgnoreCase) == 0)
				{
					NewProperty->SetPropertyFlags(Keyword.Flags);

					if (Keyword.Flags & CPF_RepNotify)
					{
						NewProperty->RepNotifyFunc = FName(*Right);
					}
				}
			}
		}
		
		return NewProperty;
	};

	auto Create = [&]() -> UProperty* {
		auto Inner = [&](UObject* Outer, const FString& Type) -> UProperty* {
			// Find TypeObject (to make UObjectHash happy)
			auto FindTypeObject = [](const TCHAR* ObjectName) {
				const TCHAR* PackagesToSearch[] = {
					TEXT("Engine"),
					TEXT("CoreUObject")
				};
				UObject* TypeObject = nullptr;
				for (auto PackageToSearch : PackagesToSearch)
				{
					TypeObject = StaticFindObject(UObject::StaticClass(), ANY_PACKAGE, *FString::Printf(TEXT("/Script/%s.%s"), PackageToSearch, ObjectName));
					if (TypeObject) return TypeObject;
				}
				return StaticFindObject(UObject::StaticClass(), ANY_PACKAGE, ObjectName);
			};

			UObject* TypeObject = FindTypeObject(*Type);

			if (auto p = Cast<UClass>(TypeObject))
			{
				auto q = NewObject<UObjectProperty>(Outer, Name);
				q->SetPropertyClass(p);
				return q;
			}
			else if (auto p = Cast<UScriptStruct>(TypeObject))
			{
				auto q = NewObject<UStructProperty>(Outer, Name);
				q->Struct = p;
				return q;
			}
			else if (Type == FString("bool"))
			{
				auto q = NewObject<UBoolProperty>(Outer, Name);
				return q;
			}
			else if (Type == FString("int"))
			{
				auto q = NewObject<UIntProperty>(Outer, Name);
				return q;
			}
			else if (Type == FString("string"))
			{
				auto q = NewObject<UStrProperty>(Outer, Name);
				return q;
			}
			else if (Type == FString("float"))
			{
				auto q = NewObject<UFloatProperty>(Outer, Name);
				return q;
			}
			else
			{
				auto q = NewObject<UInt64Property>(Outer, Name);
				return q;
			}
		};

		if (Type.EndsWith(TEXT("[]"))) 
		{
			auto q = NewObject<UArrayProperty>(Outer, Name);
			q->Inner = Inner(q, Type.Left(Type.Len()-2));
			return q;
		} 
		else
		{
			return Inner(Outer, Type);
		}
	};

	return SetupProperty(Create());
};

static UProperty* DuplicateProperty(UObject* Outer, UProperty* Property, FName Name) 
{
	auto SetupProperty = [&](UProperty* NewProperty) {
		NewProperty->SetPropertyFlags(Property->GetPropertyFlags());
		return NewProperty;
	};

	auto Clone = [&]() -> UProperty* {
		if (auto p = Cast<UStructProperty>(Property))
		{
			auto q = NewObject<UStructProperty>(Outer, Name);
			q->Struct = p->Struct;
			return q;
		}
		else if (auto p = Cast<UArrayProperty>(Property))
		{
			auto q = NewObject<UArrayProperty>(Outer, Name);
			q->Inner = DuplicateProperty(q, p->Inner, p->Inner->GetFName());
			return q;
		}		
		else if (auto p = Cast<UByteProperty>(Property))
		{
			auto q = NewObject<UByteProperty>(Outer, Name);
			q->Enum = p->Enum;
			return q;
		}
		else if (auto p = Cast<UBoolProperty>(Property))
		{
			auto q = NewObject<UBoolProperty>(Outer, Name);
			q->SetBoolSize(sizeof(bool), true);
			return q;
		}
		else if (auto p = Cast<UClassProperty>(Property))
		{
			auto q = NewObject<UClassProperty>(Outer, Name);
			q->SetMetaClass(p->MetaClass);
			q->PropertyClass = UClass::StaticClass();
			return q;
		}
		else if (auto p = Cast<UObjectProperty>(Property))
		{
			auto q = NewObject<UObjectProperty>(Outer, Name);
			q->SetPropertyClass(p->PropertyClass);
			return q;
		}
		else
		{
			return static_cast<UProperty*>(StaticDuplicateObject(Property, Outer, *(Name.ToString())));
		}
	};

	return SetupProperty(Clone());
};

void UJavascriptGeneratedFunction::Thunk(FFrame& Stack, RESULT_DECL)
{
	P_FINISH;

	auto Function = static_cast<UJavascriptGeneratedFunction*>(Stack.CurrentNativeFunction);	

	if (Function->JavascriptContext.IsValid())
	{
		auto Context = Function->JavascriptContext.Pin();

		Isolate::Scope isolate_scope(Context->isolate());
		HandleScope handle_scope(Context->isolate());
		
		bool bCallRet = Context->CallProxyFunction(Function->GetOuter(), this, Function, Stack.Locals);
		if (bCallRet == false)
			return;

		UProperty* ReturnProp = ((UFunction*)Stack.Node)->GetReturnProperty();
		if (ReturnProp != NULL)
		{
			const bool bHasReturnParam = Function->ReturnValueOffset != MAX_uint16;
			uint8* ReturnValueAdress = bHasReturnParam ? (Stack.Locals + Function->ReturnValueOffset) : nullptr;
			if (ReturnValueAdress)
				FMemory::Memcpy(RESULT_PARAM, ReturnValueAdress, ReturnProp->ArrayDim * ReturnProp->ElementSize);
		}

		bool bHasAnyOutParams = false;
		if (Function && Function->HasAnyFunctionFlags(FUNC_HasOutParms))
		{
			// Iterate over input parameters
			for (TFieldIterator<UProperty> It(Function); It && (It->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm; ++It)
			{
				// This is 'out ref'!
				if ((It->PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
				{
					bHasAnyOutParams = true;
					break;
				}
			}
		}

		if (bHasAnyOutParams)
		{
			auto OutParm = Stack.OutParms;

			// Iterate over parameters again
			for (TFieldIterator<UProperty> It(Function); It; ++It)
			{
				UProperty* Param = *It;

				auto PropertyFlags = Param->GetPropertyFlags();
				if ((PropertyFlags & (CPF_ConstParm | CPF_OutParm)) == CPF_OutParm)
				{
					auto Property = OutParm->Property;
					if (Property != nullptr)
					{
						auto ValueAddress = Property->ContainerPtrToValuePtr<uint8>(Stack.Locals);
						FMemory::Memcpy(OutParm->PropAddr, ValueAddress, Property->ArrayDim * Property->ElementSize);						
					}
				}

				if (PropertyFlags & CPF_OutParm)
					OutParm = OutParm->NextOutParm;
			}
		}
	}
}

class FJavascriptContextImplementation : public FJavascriptContext
{
	friend class UJavascriptContext;

	const FObjectInitializer* ObjectInitializer{ nullptr };

	virtual const FObjectInitializer* GetObjectInitializer() override 
	{
		return ObjectInitializer;
	}

	int32 Magic{ MagicNumber };	

	Persistent<Context> context_;
	IJavascriptDebugger* debugger{ nullptr };

	TMap<FString, UObject*> WKOs;

public:
	Isolate* isolate() { return Environment->isolate_; }
	Local<Context> context() { return Local<Context>::New(isolate(), context_); }
	bool IsValid() const { return Magic == MagicNumber; }

public:

	TMap<FString, UniquePersistent<Value>> Modules;
	TArray<FString>& Paths;

	void SetAsDebugContext()
	{
		if (debugger) return;

		Isolate::Scope isolate_scope(isolate());
		HandleScope handle_scope(isolate());

		debugger = IJavascriptDebugger::Create(5858, context());
	}

	bool IsDebugContext() const
	{
		return debugger != nullptr;
	}

	void ResetAsDebugContext()
	{
		if (debugger)
		{
			Isolate::Scope isolate_scope(isolate());
			HandleScope handle_scope(isolate());

			debugger->Destroy();
			debugger = nullptr;
		}
	}

	FJavascriptContextImplementation(TSharedPtr<FJavascriptIsolate> InEnvironment, TArray<FString>& InPaths)
		: FJavascriptContext(InEnvironment), Paths(InPaths)
	{
		Isolate::Scope isolate_scope(isolate());
		HandleScope handle_scope(isolate());

		auto context = Context::New(isolate(), nullptr, Environment->GetGlobalTemplate());
		context->SetAlignedPointerInEmbedderData(kContextEmbedderDataIndex, this);

		context_.Reset(isolate(), context);

		ExposeGlobals();

		Paths = IV8::Get().GetGlobalScriptSearchPaths();
	}

	~FJavascriptContextImplementation()
	{
		PurgeModules();

		ReleaseAllPersistentHandles();

		ResetAsDebugContext();

		context_.Reset();		
	}

	void ReleaseAllPersistentHandles()
	{
		// Release all object instances
		ObjectToObjectMap.Empty();

		// Release all struct instances
		MemoryToObjectMap.Empty();
	}

	void ExposeGlobals()
	{
		HandleScope handle_scope(isolate());
		Context::Scope context_scope(context());

		ExposeRequire();
		ExportUnrealEngineClasses();
	}

	void PurgeModules()
	{
		Modules.Empty();
	}

	void ExportUnrealEngineClasses()
	{
		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto Context = reinterpret_cast<FJavascriptContextImplementation*>((Local<External>::Cast(info.Data()))->Value());

			auto isolate = info.GetIsolate();

			FIsolateHelper I(isolate);

			HandleScope scope(isolate);

			auto Name = StringFromV8(info[0]);
			auto Opts = info[1]->ToObject();
			auto Outer = UObjectFromV8(Opts->Get(I.Keyword("Outer")));
			auto ParentClass = UClassFromV8(isolate, Opts->Get(I.Keyword("Parent")));
			Outer = Outer ? Outer : GetTransientPackage();
			ParentClass = ParentClass ? ParentClass : UObject::StaticClass();

			UBlueprintGeneratedClass* Class = nullptr;
			if (Cast<UBlueprintGeneratedClass>(ParentClass))
			{
				auto Klass = NewObject<UJavascriptGeneratedClass>(Outer, *Name, RF_Public);
				Klass->JavascriptContext = Context->AsShared();				
				Class = Klass;
			}
			else 
			{
				auto Klass = NewObject<UJavascriptGeneratedClass_Native>(Outer, *Name, RF_Public);
				Klass->JavascriptContext = Context->AsShared();
				Class = Klass;

				// This flag is necessary for proper initialization
				Class->ClassFlags |= CLASS_Native;
			}

			// Create a blueprint
			auto Blueprint = NewObject<UBlueprint>(Outer);
			Blueprint->GeneratedClass = Class;
			Class->ClassGeneratedBy = Blueprint;			

			auto ClassConstructor = [](const FObjectInitializer& ObjectInitializer){
				auto Class = static_cast<UBlueprintGeneratedClass*>(ObjectInitializer.GetClass());
				Class->GetSuperClass()->ClassConstructor(ObjectInitializer);

				FJavascriptContextImplementation* Context = nullptr;

				if (auto Klass = Cast<UJavascriptGeneratedClass_Native>(Class)) 
				{
					if (Klass->JavascriptContext.IsValid())
					{
						Context = static_cast<FJavascriptContextImplementation*>(Klass->JavascriptContext.Pin().Get());
					}
				}
				else if (auto Klass = Cast<UJavascriptGeneratedClass>(Class))
				{
					if (Klass->JavascriptContext.IsValid())
					{
						Context = static_cast<FJavascriptContextImplementation*>(Klass->JavascriptContext.Pin().Get());
					}
				}


				if (Context)
				{
					auto isolate = Context->isolate();
					auto Object = ObjectInitializer.GetObj();

					FIsolateHelper I(isolate);

					Isolate::Scope isolate_scope(isolate);
					HandleScope handle_scope(isolate);
					Context::Scope context_scope(Context->context());

					auto Holder = Context->ExportObject(Class);					

					auto v8_obj = Holder->ToObject();
					auto proxy = v8_obj->Get(I.Keyword("proxy"));
					if (proxy.IsEmpty() || !proxy->IsObject())
					{
						I.Throw(TEXT("Invalid proxy : construct class"));
						return;
					}
					auto func = proxy->ToObject()->Get(I.Keyword("ctor"));					

					Context->ObjectInitializer = &ObjectInitializer;

					auto This = Context->ExportObject(Object);

					auto context = Context->context();

					if (func->IsFunction())
					{
						CallJavascriptFunction(context, This, nullptr, Local<Function>::Cast(func), nullptr);
					}

					Context->ObjectInitializer = nullptr;
				}
			};

			Class->ClassConstructor = ClassConstructor;
			
			// Set properties we need to regenerate the class with
			Class->PropertyLink = ParentClass->PropertyLink;
			Class->ClassWithin = ParentClass->ClassWithin;
			Class->ClassConfigName = ParentClass->ClassConfigName;
			
			Class->SetSuperStruct(ParentClass);
			Class->ClassFlags |= (ParentClass->ClassFlags & (CLASS_Inherit | CLASS_ScriptInherit | CLASS_CompiledFromBlueprint));
			Class->ClassCastFlags |= ParentClass->ClassCastFlags;			

			auto AddFunction = [&](FName NewFunctionName, Handle<Value> TheFunction) -> bool {
				UFunction* ParentFunction = ParentClass->FindFunctionByName(NewFunctionName);
				
				UJavascriptGeneratedFunction* Function{ nullptr };

				auto MakeFunction = [&]() {
					Function = NewObject<UJavascriptGeneratedFunction>(Class, NewFunctionName, RF_Public);
					Function->JavascriptContext = Context->AsShared();
					Function->RepOffset = MAX_uint16;
					Function->ReturnValueOffset = MAX_uint16;
					Function->FirstPropertyToInit = NULL;

					Function->Script.Add(EX_Nothing);
				};

				// Overridden function should have its parent function
				if (ParentFunction)
				{				
					MakeFunction();

					Function->SetSuperStruct(ParentFunction);					

					auto InitializeProperties = [](UFunction* Function, UFunction* ParentFunction) {
						UField** Storage = &Function->Children;
						UProperty** PropertyStorage = &Function->PropertyLink;

						for (TFieldIterator<UProperty> PropIt(ParentFunction, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
						{
							UProperty* Property = *PropIt;
							if (Property->HasAnyPropertyFlags(CPF_Parm))
							{
								UProperty* NewProperty = DuplicateProperty(Function, Property, Property->GetFName());

								*Storage = NewProperty;
								Storage = &NewProperty->Next;

								*PropertyStorage = NewProperty;
								PropertyStorage = &NewProperty->PropertyLinkNext;
							}
						}
					};

					if (ParentFunction)
					{
						InitializeProperties(Function, ParentFunction);
					}
				}
				else
				{
					TArray<FString> Arguments;

					{
						FString TheString = StringFromV8(TheFunction->ToString());

						int32 Index;						

						if (TheString.FindChar('(', Index))
						{
							TheString = TheString.Mid(Index + 1);
						}

						if (TheString.FindChar(')', Index))
						{
							FString Rest = TheString.Mid(Index + 1);
							TheString = TheString.Left(Index);

							if (Rest.FindChar('{', Index))
							{
								Rest = Rest.Left(Index);

								TArray<FString> SmallArray;
								const TCHAR* Delims[] = {
									TEXT("/*"),
									TEXT("*/"),
									TEXT("+")
								};

								static struct FKeyword {
									const TCHAR* Keyword;
									int32 Flags;
								} Keywords[] = {									
									{ TEXT("Exec"), FUNC_Exec },
									{ TEXT("Server"), FUNC_Net | FUNC_NetServer},
									{ TEXT("Client"), FUNC_Net | FUNC_NetClient },
									{ TEXT("NetMulticast"), FUNC_Net | FUNC_NetMulticast },
									{ TEXT("Reliable"), FUNC_NetReliable },
									{ TEXT("Unreliable"), FUNC_NetResponse }
								};								
									
								Rest.Trim().ParseIntoArray(SmallArray, Delims, ARRAY_COUNT(Delims));

								// If this function is not a UFunction-candiate?
								if (SmallArray.Num() == 0 && !TheString.Contains(TEXT("/*")))
								{
									return false;
								}

								MakeFunction();
								
								for (const auto& Flag : SmallArray)
								{
									for (const auto& Keyword : Keywords)
									{
										// Partial match is allowed
										if (Flag.Contains(Keyword.Keyword))
										{
											Function->FunctionFlags |= Keyword.Flags;
										}
									}
								}
							}							
						}

						TArray<FString> Array;
						TheString.ParseIntoArray(Array, TEXT(","));

						for (auto Item : Array)
						{
							Arguments.Add(Item);							
						}
					}

					auto InitializeProperties = [](UFunction* Function, const TArray<FString>& Arguments) {
						UField** Storage = &Function->Children;
						UProperty** PropertyStorage = &Function->PropertyLink;

						for (const auto& Argument : Arguments)
						{
							UProperty* NewProperty = CreateProperty(Function, Argument);

							if (NewProperty)
							{
								NewProperty->SetPropertyFlags(CPF_Parm);
								
								*Storage = NewProperty;
								Storage = &NewProperty->Next;

								*PropertyStorage = NewProperty;
								PropertyStorage = &NewProperty->PropertyLinkNext;								
							}
							else
							{
								// ??
							}
						}
					};

					InitializeProperties(Function, Arguments);					
				}

				auto FinalizeFunction = [](UFunction* Function) {
					Function->Bind();
					Function->StaticLink(true);

					Function->FunctionFlags |= FUNC_Native;

					for (TFieldIterator<UProperty> PropIt(Function, EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
					{
						UProperty* Property = *PropIt;
						if (Property->HasAnyPropertyFlags(CPF_Parm))
						{
							++Function->NumParms;
							Function->ParmsSize = Property->GetOffset_ForUFunction() + Property->GetSize();

							if (Property->HasAnyPropertyFlags(CPF_OutParm))
							{
								Function->FunctionFlags |= FUNC_HasOutParms;
							}

							if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
							{
								Function->ReturnValueOffset = Property->GetOffset_ForUFunction();

								if (!Property->HasAnyPropertyFlags(CPF_IsPlainOldData | CPF_NoDestructor))
								{
									Property->DestructorLinkNext = Function->DestructorLink;
									Function->DestructorLink = Property;
								}
							}
						}
						else
						{
							if (!Property->HasAnyPropertyFlags(CPF_ZeroConstructor))
							{
								Function->FirstPropertyToInit = Property;
								Function->FunctionFlags |= FUNC_HasDefaults;
								break;
							}
						}
					}
				};

				FinalizeFunction(Function);

				Function->SetNativeFunc((Native)&UJavascriptGeneratedFunction::Thunk);

				Function->Next = Class->Children;
				Class->Children = Function;

				// Add the function to it's owner class function name -> function map
				Class->AddFunctionToFunctionMap(Function);

				return true;
			};

			auto Functions = Opts->Get(I.Keyword("Functions"));
			TMap<FString,Handle<Value>> Others;
			if (!Functions.IsEmpty() && Functions->IsObject())
			{
				auto FuncMap = Functions->ToObject();
				auto Keys = FuncMap->GetOwnPropertyNames();

				auto NumKeys = Keys->Length();

				for (decltype(NumKeys) Index = 0; Index < NumKeys; ++Index)
				{
					auto Name = Keys->Get(Index);
					auto UName = StringFromV8(Name);
					auto Function = FuncMap->Get(Name);

					if (!Function->IsFunction()) continue;

					if (UName == TEXT("properties"))
					{						
						FString Code = StringFromV8(Function->ToString());

						TArray<FString> SmallArray;
						const TCHAR* Delims[] = {
							TEXT("\n"),
							TEXT(";")
						};
						
						Code.ParseIntoArray(SmallArray, Delims, ARRAY_COUNT(Delims));

						for (auto Line : SmallArray)
						{
							Line = Line.Trim();

							if (Line.StartsWith(TEXT("this.")))
							{
								auto Property = CreateProperty(Class, Line.Mid(5));
								if (Property)
								{
									Class->AddCppProperty(Property);

									if (Property->HasAnyPropertyFlags(CPF_Net))
									{
										Class->NumReplicatedProperties++;
									}
								}
							}
						}
					}
					else if (UName != TEXT("ctor") && UName != TEXT("constructor"))
					{
						if (!AddFunction(*UName, Function))
						{						
							Others.Add(UName, Function);
						}
					}
				}
			}

			Class->Bind();
			Class->StaticLink(true);

			{
				auto FinalClass = Context->Environment->ExportClass(Class,false);
				auto Prototype = FinalClass->PrototypeTemplate();

				for (auto It = Others.CreateIterator(); It; ++It)
				{
					Prototype->Set(I.Keyword(It.Key()), It.Value());
				}				

				Context->Environment->RegisterClass(Class, FinalClass);
			}

			auto FinalClass = Context->ExportObject(Class);
			FinalClass->ToObject()->Set(I.Keyword("proxy"), Functions);

			info.GetReturnValue().Set(FinalClass);

			// Make sure CDO is ready for use
			Class->GetDefaultObject();
		};

		auto global = context()->Global();
		auto self = External::New(isolate(), this);

		global->Set(V8_KeywordString(isolate(), "CreateClass"), FunctionTemplate::New(isolate(), fn, self)->GetFunction());
	}

	void ExposeRequire()
	{
		auto fn = [](const FunctionCallbackInfo<Value>& info) {
			auto isolate = info.GetIsolate();
			HandleScope scope(isolate);

			if (info.Length() != 1 || !(info[0]->IsString()))
			{
				return;
			}

			auto Self = reinterpret_cast<FJavascriptContextImplementation*>((Local<External>::Cast(info.Data()))->Value());

			auto required_module = StringFromV8(info[0]);

			bool found = false;

			auto inner = [&](const FString& script_path)
			{				
				auto it = Self->Modules.Find(script_path);
				if (it)
				{
					info.GetReturnValue().Set(Local<Value>::New(isolate, *it));
					found = true;
					return true;
				}

				FString Text;
				if (FFileHelper::LoadFileToString(Text, *script_path))
				{
					Text = FString::Printf(TEXT("(function (__dirname) {\nvar module = { exports : {}, filename : __dirname }, exports = module.exports;\n%s\n;return module.exports;}('%s'));"), *Text, *script_path);
					auto full_path = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*script_path);
#if PLATFORM_WINDOWS
					full_path = full_path.Replace(TEXT("/"), TEXT("\\"));
#endif
					auto exports = Self->RunScript(full_path, Text, 2);
					if (exports.IsEmpty())
					{
						UE_LOG(Javascript, Log, TEXT("Invalid script for require"));
					}
					Self->Modules.Add(script_path, UniquePersistent<Value>(isolate, exports));
					info.GetReturnValue().Set(exports);
					found = true;
					return true;
				}

				return false;
			};

			auto inner_package = [&](const FString& script_path)
			{
				FString Text;
				if (FFileHelper::LoadFileToString(Text, *(script_path / TEXT("package.json"))))
				{
					Text = FString::Printf(TEXT("(function (json) {return json.main;})(%s);"), *Text);
					auto full_path = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*script_path);
#if PLATFORM_WINDOWS
					full_path = full_path.Replace(TEXT("/"), TEXT("\\"));
#endif
					auto exports = Self->RunScript(full_path, Text, 0);
					if (exports.IsEmpty() || !exports->IsString())
					{
						return false;
					}
					else
					{
						return inner(script_path / StringFromV8(exports));
					}
				}

				return false;
			};

			auto inner2 = [&](FString base_path)
			{
				for (;;)
				{
					if (!FPaths::DirectoryExists(base_path)) return false;

					auto script_path = base_path / required_module;
					if (!script_path.EndsWith(TEXT(".js")))
					{
						if (inner(script_path + TEXT(".js"))) return true;
						if (inner(script_path / TEXT("index.js"))) return true;

						if (inner_package(script_path)) return true;
					}
					else
					{
						if (inner(script_path)) return true;
					}

					base_path = base_path / TEXT("node_modules");
				}
			};

			auto current_script_path = FPaths::GetPath(StringFromV8(StackTrace::CurrentStackTrace(isolate, 1, StackTrace::kScriptName)->GetFrame(0)->GetScriptName()));
#if PLATFORM_WINDOWS
			current_script_path = current_script_path.Replace(TEXT("\\"), TEXT("/"));
#endif
			if (!inner2(current_script_path))
			{
				for (const auto& path : Self->Paths)
				{
					if (inner2(path)) break;
				}
			}

			if (!found)
			{
				info.GetReturnValue().Set(Undefined(isolate));
			}
		};

		auto fn2 = [](const FunctionCallbackInfo<Value>& info) {
			auto isolate = info.GetIsolate();
			HandleScope scope(isolate);

			auto Self = reinterpret_cast<FJavascriptContextImplementation*>((Local<External>::Cast(info.Data()))->Value());
			Self->PurgeModules();
		};

		auto global = context()->Global();
		auto self = External::New(isolate(), this);

		global->Set(V8_KeywordString(isolate(), "require"), FunctionTemplate::New(isolate(), fn, self)->GetFunction());
		global->Set(V8_KeywordString(isolate(), "purge_modules"), FunctionTemplate::New(isolate(), fn2, self)->GetFunction());

		auto getter = [](Local<String> property, const PropertyCallbackInfo<Value>& info) {
			auto isolate = info.GetIsolate();
			HandleScope scope(isolate);

			auto Self = reinterpret_cast<FJavascriptContextImplementation*>((Local<External>::Cast(info.Data()))->Value());

			auto out = Object::New(isolate);

			for (auto it = Self->Modules.CreateConstIterator(); it; ++it)
			{
				const auto& name = it.Key();
				const auto& module = it.Value();

				auto FullPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*name);
				out->Set(V8_String(isolate, name), V8_String(isolate, TCHAR_TO_UTF8(*FullPath)));
			}

			info.GetReturnValue().Set(out);
		};
		global->SetAccessor(V8_KeywordString(isolate(), "modules"), getter, 0, self);
	}

	FString GetScriptFileFullPath(const FString& Filename)
	{
		for (auto Path : Paths)
		{
			auto FullPath = Path / Filename;
			auto Size = IFileManager::Get().FileSize(*FullPath);
			if (Size != INDEX_NONE)
			{
				return IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*FullPath);
			}
		}
		return Filename;
	}

	FString ReadScriptFile(const FString& Filename)
	{
		auto Path = GetScriptFileFullPath(Filename);

		FString Text;

		FFileHelper::LoadFileToString(Text, *Path);

		return Text;
	}

	Local<Value> RunFile(const FString& Filename)
	{
		HandleScope handle_scope(isolate());

		auto Script = ReadScriptFile(Filename);

		return RunScript(GetScriptFileFullPath(Filename), Script);
	}

	void Public_RunFile(const FString& Filename)
	{
		RunFile(Filename);
	}

	FString Public_RunScript(const FString& Script, bool bOutput = true)
	{
		Isolate::Scope isolate_scope(isolate());
		HandleScope handle_scope(isolate());
		Context::Scope context_scope(context());

		auto ret = RunScript(TEXT("(inline)"), Script);
		auto str = ret.IsEmpty() ? TEXT("(empty)") : StringFromV8(ret);

		if (bOutput && !ret.IsEmpty())
		{
			UE_LOG(Javascript, Log, TEXT("%s"), *str);
		}		
		return str;
	}

	// Should be guarded with proper handle scope
	Local<Value> RunScript(const FString& Filename, const FString& Script, int line_offset = 0)
	{
		Isolate::Scope isolate_scope(isolate());
		Context::Scope context_scope(context());

		TryCatch try_catch;
		try_catch.SetVerbose(true);

		auto Path = Filename;
		auto source = V8_String(isolate(), Script);
		auto path = V8_String(isolate(), Path);
		ScriptOrigin origin(path, Integer::New(isolate(), -line_offset));
		auto script = Script::Compile(source, &origin);

		auto result = script->Run();
		if (try_catch.HasCaught())
		{
			FV8Exception::Report(try_catch);
			return Local<Value>();
		}
		else
		{
			return result;
		}
	}

	void Expose(FString RootName, UObject* Object)
	{
		WKOs.Add(RootName, Object);

		auto RootGetter = [](Local<String> property, const PropertyCallbackInfo<Value>& info) {
			auto isolate = info.GetIsolate();
			info.GetReturnValue().Set(info.Data());
		};

		Isolate::Scope isolate_scope(isolate());
		HandleScope handle_scope(isolate());
		Context::Scope context_scope(context());

		context()->Global()->SetAccessor(V8_KeywordString(isolate(), RootName), RootGetter, 0, ExportObject(Object));
	}

	Local<Value> ExportObject(UObject* Object, bool bForce = false) override
	{
		return Environment->ExportObject(Object, bForce);
	}

	bool WriteAliases(const FString& Filename)
	{
#if WITH_EDITOR
		struct TokenWriter
		{
			FString Text;

			TokenWriter& push(const char* something)
			{
				Text.Append(ANSI_TO_TCHAR(something));
				return *this;
			}

			TokenWriter& push(const FString& something)
			{
				Text.Append(something);
				return *this;
			}

			const TCHAR* operator * ()
			{
				return *Text;
			}
		};

		TokenWriter w;

		for (auto it = Environment->ClassToFunctionTemplateMap.CreateConstIterator(); it; ++it)
		{
			const UClass* ClassToExport = it.Key();

			// Skip a generated class
			if (ClassToExport->ClassGeneratedBy) continue;

			auto ClassName = FV8Config::Safeify(ClassToExport->GetName());

			TArray<UFunction*> Functions;
			Environment->BlueprintFunctionLibraryMapping.MultiFind(ClassToExport, Functions);

			auto conditional_emit_alias = [&](UFunction* Function, bool is_thunk) {
				auto Alias = FV8Config::GetAlias(Function);
				if (FV8Config::CanExportFunction(ClassToExport, Function) && Alias.Len() > 0)
				{
					w.push(ClassName);
					w.push(".prototype.");
					w.push(Alias);
					w.push(" = ");
					w.push(ClassName);
					w.push(".prototype.");
					w.push(FV8Config::Safeify(Function->GetName()));
					w.push(";\n");

					if (!is_thunk && Function->FunctionFlags & FUNC_Static)
					{
						w.push(ClassName);
						w.push(".");
						w.push(Alias);
						w.push(" = ");
						w.push(ClassName);
						w.push(".");
						w.push(FV8Config::Safeify(Function->GetName()));
						w.push(";\n");
					}
				}
			};

			for (auto Function : Functions)
			{
				conditional_emit_alias(Function, true);
			}

			for (TFieldIterator<UFunction> FuncIt(ClassToExport, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
			{
				conditional_emit_alias(*FuncIt, false);
			}
		}

		return FFileHelper::SaveStringToFile(*w, *Filename);
#else
		return false;
#endif
	}

	bool WriteDTS(const FString& Filename)
	{
#if WITH_EDITOR
		TypingGenerator instance(*(Environment.Get()));

		instance.ExportBootstrap();

		for (auto it = Environment->ClassToFunctionTemplateMap.CreateConstIterator(); it; ++it)
		{
			instance.Export(it.Key());
		}

		for (auto pair : WKOs)
		{
			auto k = pair.Key;
			auto v = pair.Value;

			instance.ExportWKO(k, v);
		}

		instance.Finalize();

		return instance.Save(Filename);
#else
		return false;
#endif
	}

	Local<Value> GetProxyFunction(UObject* Object, const TCHAR* Name)
	{
		auto v8_obj = ExportObject(Object)->ToObject();
		auto proxy = v8_obj->Get(V8_KeywordString(isolate(), "proxy"));
		if (proxy.IsEmpty() || !proxy->IsObject())
		{
			return Undefined(isolate());
		}

		auto func = proxy->ToObject()->Get(V8_KeywordString(isolate(), Name));
		if (func.IsEmpty() || !func->IsFunction())
		{
			return Undefined(isolate());
		}

		return func;
	}

	Local<Value> GetProxyFunction(UObject* Object, UFunction* Function)
	{
		return GetProxyFunction(Object, *FV8Config::Safeify(Function->GetName()));
	}

	bool HasProxyFunction(UObject* Holder, UFunction* Function)
	{
		Isolate::Scope isolate_scope(isolate());
		HandleScope handle_scope(isolate());

		auto func = GetProxyFunction(Holder, Function);;
		return !func.IsEmpty() && func->IsFunction();
	}

	bool CallProxyFunction(UObject* Holder, UObject* This, UFunction* FunctionToCall, void* Parms)
	{
		Isolate::Scope isolate_scope(isolate());
		HandleScope handle_scope(isolate());

		Context::Scope context_scope(context());

		auto func = GetProxyFunction(Holder, FunctionToCall);
		if (!func.IsEmpty() && func->IsFunction())
		{
			CallJavascriptFunction(context(), This ? ExportObject(This) : context()->Global(), FunctionToCall, Local<Function>::Cast(func), Parms);

			return true;
		}
		else
		{
			return false;
		}
	}

	// To tell Unreal engine's GC not to destroy these objects!
	virtual void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector) override
	{		
		Public_RunScript(TEXT("gc();"), false);

		// All objects
		for (auto It = ObjectToObjectMap.CreateIterator(); It; ++It)
		{
			Collector.AddReferencedObject(It.Key(), InThis);
		}
	}
};

FJavascriptContext* FJavascriptContext::FromV8(v8::Local<v8::Context> Context)
{
	if (Context.IsEmpty()) return nullptr;
	
	auto Instance = reinterpret_cast<FJavascriptContextImplementation*>(Context->GetAlignedPointerFromEmbedderData(kContextEmbedderDataIndex));
	if (Instance->IsValid())
	{
		return Instance;
	}
	else
	{
		return nullptr;
	}
}

FJavascriptContext* FJavascriptContext::Create(TSharedPtr<FJavascriptIsolate> InEnvironment, TArray<FString>& InPaths)
{
	return new FJavascriptContextImplementation(InEnvironment, InPaths);
}
